#include "app/config.h"
#include "signal_handler.h"
#include "rtsp_server.h"
#include "common/log.h"
#include <gst/gst.h>
#include <getopt.h>
#include <string>
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

    RtspServer server;
    if (!server.start(cfg)) {
        g_main_loop_unref(loop);
        return 2;
    }

    g_main_loop_run(loop);             // 阻塞，SIGINT/SIGTERM 退出

    server.stop();
    g_main_loop_unref(loop);
    LOGI("iotcam exited cleanly");
    return 0;
}