#include "app/config.h"
#include "signal_handler.h"
#include "rtsp_server.h"
#include "shader_filter.h"
#include "control_channel.h"
#include "common/log.h"
#include <gst/gst.h>
#include <getopt.h>
#include <string>
#include <filesystem>
#include <cstdlib>


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

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    install_signals(loop);

    /* GL 滤镜模块：独立于 RtspServer，同时被 RtspServer（注入 fragment）
     * 与 ControlChannel（热切换 uniform）共享。如 filter.enabled=false，
     * 则向 RtspServer 传 nullptr，pipeline 不插 GL 段。 */
    ShaderFilter filter;
    if (cfg.filter.enabled) {
        std::string shaders_base = (std::filesystem::path(cfg.config_dir) / ".." / "shaders")
                                   .lexically_normal().string();
        filter.configure(cfg.filter, shaders_base);
    }

    RtspServer server;
    if (!server.start(cfg, cfg.filter.enabled ? &filter : nullptr)) {
        g_main_loop_unref(loop);
        return 2;
    }

    /* FIFO 命令通道：运行时热切换 filter_type / reload shader。 */
    ControlChannel ctrl;
    ctrl.start(cfg.filter.control_fifo, cfg.filter.enabled ? &filter : nullptr);

    g_main_loop_run(loop);             // 阻塞，SIGINT/SIGTERM 退出

    ctrl.stop();
    server.stop();
    filter.shutdown();
    g_main_loop_unref(loop);
    LOGI("iotcam exited cleanly");
    return 0;
}