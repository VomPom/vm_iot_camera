#include "config.h"
#include "signal_handler.h"
#include "rtsp_server.h"
#include "shader_filter.h"
#include "branch_base.h"
#include "snapshot.h"
#include "record.h"
#include "control_channel.h"
#include "log.h"
#include "v4l2_prober.h"
#include <gst/gst.h>
#include <getopt.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

/* 第一遍只取 --config，便于 yaml -> CLI 的覆盖顺序正确 */
static std::string extract_config_path(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "-c" || a == "--config") return argv[i + 1];
    }
    return "config/default.yaml";
}

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    /* 启动时间用于 status 命令计算 uptime；steady_clock 不受系统时间调整影响。 */
    auto start_time = std::chrono::steady_clock::now();

    Config cfg;
    try {
        cfg = Config::from_file(extract_config_path(argc, argv));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "load config failed: %s\n", e.what());
        return 1;
    }
    cfg.merge_cli(argc, argv);

    setup_logger(cfg.log.level);
    LOGI("iotcam starting, encoder={}, device={}, {}x{}@{}",
         cfg.encoder.backend, cfg.capture.device,
         cfg.capture.width, cfg.capture.height, cfg.capture.framerate);

    /* 启动前快速验证摄像头设备是否存在且可访问。
     * 如果设备没插 / 权限不够，这里直接返回人类可读错误并退出， */
    if (!v4l2_prober::device_accessible(cfg.capture.device))
    {
        LOGE("camera device '{}' not accessible: {}",
             cfg.capture.device, std::strerror(errno));
        std::fprintf(stderr,
                     "\n"
                     "=============================================\n"
                     "  ERROR: Camera device not accessible\n"
                     "  Device: %s\n"
                     "  Reason: %s\n"
                     "=============================================\n"
                     "  → Is the camera plugged in?\n"
                     "  → Does the device path exist? (ls -l %s)\n"
                     "  → Do you have read/write permission?\n"
                     "     (sudo usermod -aG video $USER && re-login)\n"
                     "=============================================\n\n",
                     cfg.capture.device.c_str(),
                     std::strerror(errno),
                     cfg.capture.device.c_str());
        return 3;
    }

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    install_signals(loop);

    /* GL 滤镜模块：独立于 RtspServer，同时被 RtspServer（注入 fragment）
     * 与 ControlChannel（热切换 uniform）共享。如 filter.enabled=false，
     * 则向 RtspServer 传 nullptr，pipeline 不插 GL 段。 */
    ShaderFilter filter;
    if (cfg.filter.enabled)
    {
        std::string shaders_base = (std::filesystem::path(cfg.config_dir) / ".." / "shaders")
                                   .lexically_normal().string();
        filter.configure(cfg.filter, shaders_base);
    }

    RtspServer server;
    Snapshot   snapshot;
    Record     record;
    /* 截图副线与其他副线平级：独立于 RtspServer，只需 RtspServer 在 media-configure
     * 时帮它 attach。输出目录 / 质量 / 超时都由 cfg.snapshot 供给。 */
    snapshot.configure(cfg.snapshot.dir, cfg.snapshot.quality, cfg.snapshot.timeout_ms);
    /* 录像副线同样是独立模块：media-configure 时抢 valve/sink，按 cfg.enabled 决定开闸。 */
    record.configure(cfg.record);

    /* 把所有 branch 实例统一成 BranchBase* 列表交给 RtspServer。
     * 顺序无强约束（每个 branch 抓自己的命名元素，互不干扰）；未来新增 detect / cloud_upload
     * 之类副线时只需在此 push 一个新对象，RtspServer 不用改。 */
    std::vector<BranchBase*> branches{&snapshot, &record};

    if (!server.start(cfg, cfg.filter.enabled ? &filter : nullptr, branches))
    {
        g_main_loop_unref(loop);
        return 2;
    }

    /* FIFO 命令通道：运行时热切换 filter_type / reload shader / 查询状态。
     * - 请求 FIFO（control.request_fifo）必填才会监听；
     * - 应答 FIFO（control.reply_fifo）选填，配置后命令会有结构化应答。
     * - 传入 cfg/server/t0 让 status 命令能取到运行时与配置信息。 */
    ControlChannel ctrl;
    ctrl.start(cfg.control.request_fifo,
               cfg.control.reply_fifo,
               cfg.filter.enabled ? &filter : nullptr,
               &cfg,
               &server,
               &snapshot,
               &record,
               start_time);

    g_main_loop_run(loop);             // 阻塞，SIGINT/SIGTERM 退出

    /* 退出顺序很关键：
     *   1) 先停 ControlChannel：避免 FIFO 命令在 shutdown 期间访问已释放对象。
     *   2) 再停所有 branch：record 在 shutdown 时若仍在录，会向 rec_queue 注入
     *      EOS 让 splitmuxsink 把当前段 finalize（写 moov box），并等
     *      "splitmuxsink-fragment-closed" 消息（≤1.5s 超时）。此时 RTSP media/
     *      pipeline 还活着，bus 与元素引用都未回收，EOS 才能正确传播。
     *   3) 最后停 RtspServer / filter：释放 server 与共享资源。 */
    ctrl.stop();
    snapshot.shutdown();
    record.shutdown();
    server.stop();
    filter.shutdown();
    g_main_loop_unref(loop);
    LOGI("iotcam exited cleanly");
    return 0;
}