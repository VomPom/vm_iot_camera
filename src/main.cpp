#include "config.h"
#include "signal_handler.h"
#include "rtsp_server.h"
#include "shader_filter.h"
#include "branch_base.h"
#include "snapshot.h"
#include "pag_branch.h"
#include "pag_sticker.h"
#include "pag_effect.h"
#include "control_channel.h"
#include "event_fifo_writer.h"
#include "face_branch.h"
#include "face_prober.h"
#include <memory>
#include "log.h"
#include "v4l2_prober.h"
#include "gstpagfilter.h"
#include "pag_sdk.h"
#include <gst/gst.h>
#include <getopt.h>
#include <chrono>
#include <cerrno>
#include <cstdlib>
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
    return "assets/config/default.yaml";
}

int main(int argc, char** argv) {
    /* headless Pi EGL 兼容：
     *
     * 树莓派/Ubuntu 在没有 X/Wayland 会话时，Mesa 的 EGL 默认平台会按
     * `DISPLAY` 推断到 x11 后端，导致 libpag 内部 `eglInitialize` /
     * `eglCreatePbufferSurface` 在流线程上首次调用时返回 12289
     * (EGL_NOT_INITIALIZED)，最终 `PAGSurface::MakeOffscreen` 失败、
     * pagfilter 静默退化为 passthrough。
     *
     * 这里强制指定 `surfaceless` 平台，让 Mesa 走 EGL_PLATFORM_SURFACELESS
     * 路径——纯离屏渲染不依赖任何窗口系统。第三参数 `0` 表示「已设置则
     * 不覆盖」，保留运维通过环境变量手动覆盖的能力（例如本地接显示器
     * 调试时设 `EGL_PLATFORM=wayland`）。
     *
     * 必须放在 gst_init 之前：GStreamer 的某些插件（如 gst-gl）会在
     * gst_init 期间探测 EGL，时机更早能避免极少数 race。同时 libpag 在
     * 流线程首次 Engine::Make 时才真正用到 EGL，环境变量在主线程预先
     * 设置后，所有派生线程都会继承。 */
    ::setenv("EGL_PLATFORM", "surfaceless", 0);

    gst_init(&argc, &argv);

    /* 静态注册项目自研 GStreamer 元素（目前仅 pagfilter）。
     * 顺序上必须在 gst_init 之后、build pipeline 之前调用。
     * pagfilter 未注册时，launch 中出现 "pagfilter" 会被当作未知元素报错。 */
    if (!pagfilter_register_static()) {
        std::fprintf(stderr, "pagfilter: static plugin register failed\n");
        return 4;
    }

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
    LOGI("pagfilter: enabled={} selftest={} file='{}'",
         cfg.filter.pag.enabled, cfg.filter.pag.selftest, cfg.filter.pag.file);

    /* PAG SDK 版本日志 + 按需 selftest。
     * - 版本日志总是打印一次，便于排查"线上跑的到底是 stub 还是真 libpag"；
     * - selftest 仅在 cfg.filter.pag.selftest=true 时执行，独立于 pipeline。
     *   传入的路径若为相对路径，与 shaders 同样以 cfg.config_dir/.. 为基目录解析，
     *   保证从仓库根 / build 目录两种工作目录启动时都找得到 pag/PAG_LOGO.pag。 */
    LOGI("pag_sdk: enabled_at_compile={} version='{}'",
         pag_sdk::is_enabled(), pag_sdk::sdk_version());
    if (cfg.filter.pag.selftest) {
        std::string pag_path = cfg.filter.pag.file;
        if (!pag_path.empty() &&
            !std::filesystem::path(pag_path).is_absolute()) {
            pag_path = (std::filesystem::path(cfg.config_dir) / ".." / pag_path)
                           .lexically_normal().string();
        }
        const bool ok = pag_sdk::selftest_load(pag_path);
        LOGI("pag_sdk: selftest result={} resolved='{}'", ok, pag_path);
    }

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

    /* face 副线启动期检查：facedetect element 与 cascade xml 都必须就绪。
     * 仅在 cfg.face.enabled=true 时生效；失败与 v4l2/alsa 同款退出码 5。 */
    {
        std::string face_err;
        if (!face_prober::is_ready(cfg.face, &face_err))
        {
            LOGE("face_prober: {}", face_err);
            std::fprintf(stderr,
                         "\n"
                         "=============================================\n"
                         "  ERROR: Face detect branch not ready\n"
                         "  Reason: %s\n"
                         "=============================================\n"
                         "  → Disable in yaml: face.enabled: false\n"
                         "  → Or fix the underlying issue per the message above\n"
                         "=============================================\n\n",
                         face_err.c_str());
            return 5;
        }
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
    /* 截图副线与其他副线平级：独立于 RtspServer，只需 RtspServer 在 media-configure
     * 时帮它 attach。输出目录 / 质量 / 超时都由 cfg.snapshot 供给。 */
    snapshot.configure(cfg.snapshot.dir, cfg.snapshot.quality, cfg.snapshot.timeout_ms);

    /* PAG 副线：仅在 cfg.filter.pag.enabled=true 时启用。
     * PipelineBuilder 已经在主线 raw 段插入 `pagfilter name=pag0`，本模块
     * 负责在 media-configure 时把绝对化后的 pag-file 一次性注入到该元素。
     *
     * 启动期按 cfg.filter.pag.type 选具体子类：
     *   - PagType::Sticker   -> PagSticker（视频叠加，支持 position/scale）
     *   - PagType::PagEffect -> PagEffect （视频替换轨道，支持 replace-image-*）
     * 上层只持 PagBranch* 句柄；ControlChannel 用 dynamic_cast 做差异化命令分发。
     *
     * 路径解析与 selftest / shaders 一致：相对路径以 cfg.config_dir/.. 为基目录，
     * 保证从仓库根 / build 目录两种工作目录启动时都找得到资源。 */
    std::unique_ptr<PagBranch> pag_branch;
    if (cfg.filter.pag.enabled) {
        std::string pag_path = cfg.filter.pag.file;
        if (!pag_path.empty() &&
            !std::filesystem::path(pag_path).is_absolute()) {
            pag_path = (std::filesystem::path(cfg.config_dir) / ".." / pag_path)
                           .lexically_normal().string();
        }
        if (cfg.filter.pag.type == PagType::PagEffect) {
            pag_branch = std::make_unique<PagEffect>();
        } else {
            pag_branch = std::make_unique<PagSticker>();
        }
        pag_branch->configure(cfg.filter.pag, pag_path);
    }

    /* 把所有 branch 实例统一成 BranchBase* 列表交给 RtspServer。
     * 顺序无强约束（每个 branch 抓自己的命名元素，互不干扰）；未来新增 detect / cloud_upload
     * 之类副线时只需在此 push 一个新对象，RtspServer 不用改。
     * pag_branch 仅在 filter.pag.enabled=true 时入列：pipeline 里没有 pag0 元素时
     * BranchBase 会 warn 一行然后无害跳过，但提前过滤可以让日志更干净。
     */
    std::unique_ptr<FaceBranch> face_branch;
#if VM_IOT_ENABLE_FACEDETECT
    if (cfg.face.enabled) {
        face_branch = std::make_unique<FaceBranch>();
        face_branch->configure(cfg.face);
        /* 录入主线采集帧尺寸：face 副线与主线共享同一采集帧（videoscale 后），
         * facedetect 上报的坐标就在这个尺寸下，前端归一化时候需要。 */
        face_branch->set_frame_size(cfg.capture.width, cfg.capture.height);
    }
#endif

    /* 事件广播 FIFO（单向 daemon → web）：
     *   - 与控制通道完全解耦；未配置时静默禁用、不影响其他功能。
     *   - 开启且 face_branch 存在时，把人脸事件推送接到 events 写端。
     *   - main 作为 EventFifoWriter 的唯一拥有者，FaceBranch 只持回调引用；
     *     退出时先 shutdown() face_branch 断掉回调流，再 stop() events，
     *     避免 streaming 线程在 events 已释放后回头推。 */
    EventFifoWriter events;
    if (!cfg.control.event_fifo.empty()) {
        events.start(cfg.control.event_fifo);
    }
    if (face_branch && events.active()) {
        face_branch->set_on_faces_callback(
            [&events](const FaceEvent& ev, int fw, int fh) {
                events.push_faces(ev, fw, fh);
            });
    }

    std::vector<BranchBase*> branches{&snapshot};
    if (pag_branch) {
        branches.push_back(pag_branch.get());
    }
    if (face_branch) {
        branches.push_back(face_branch.get());
    }

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
               pag_branch.get(),
               face_branch.get(),
               start_time);

    g_main_loop_run(loop);             // 阻塞，SIGINT/SIGTERM 退出

    /* 退出顺序很关键：
     *   1) 先停 ControlChannel：避免 FIFO 命令在 shutdown 期间访问已释放对象。
     *   2) 再停所有 branch。
     *   3) 最后停 RtspServer / filter：释放 server 与共享资源。 */
    ctrl.stop();
    if (face_branch) {
        face_branch->shutdown();
    }
    events.stop();
    snapshot.shutdown();
    if (pag_branch) {
        pag_branch->shutdown();
    }
    server.stop();
    filter.shutdown();
    g_main_loop_unref(loop);
    LOGI("iotcam exited cleanly");
    return 0;
}