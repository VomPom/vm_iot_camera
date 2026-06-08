//
// Created by vompom on on 2026/06/05 17:04.
//
// @Description
//   解析 YAML 配置文件，缺省字段使用默认值，非法字段记录 warning 后回退默认。
//

#include "config.h"

#include <fstream>
#include <yaml-cpp/yaml.h>
#include <getopt.h>
#include <string>
#include <spdlog/spdlog.h>

Config Config::from_file(const std::string& path) {
    Config c;
    YAML::Node n = YAML::LoadFile(path);

    if (auto s = n["server"]) {
        c.server.port  = s["port"].as<uint16_t>(c.server.port);
        c.server.mount = s["mount"].as<std::string>(c.server.mount);
    }
    if (auto cap = n["capture"]) {
        c.capture.device    = cap["device"   ].as<std::string>(c.capture.device);
        c.capture.width     = cap["width"    ].as<int>        (c.capture.width);
        c.capture.height    = cap["height"   ].as<int>        (c.capture.height);
        c.capture.framerate = cap["framerate"].as<int>        (c.capture.framerate);
        c.capture.pixfmt    = cap["pixfmt"   ].as<std::string>(c.capture.pixfmt);
    }
    if (auto e = n["encoder"]) {
        c.encoder.backend      = e["backend"     ].as<std::string>(c.encoder.backend);
        c.encoder.bitrate_kbps = e["bitrate_kbps"].as<int>(c.encoder.bitrate_kbps);
        c.encoder.gop          = e["gop"         ].as<int>(c.encoder.gop);
        c.encoder.bframes      = e["bframes"     ].as<int>(c.encoder.bframes);
    }
    if (auto l = n["log"]) {
        c.log.level = l["level"].as<std::string>(c.log.level);
    }
    // todo:: read pipeline config.
    if (auto p = n["pipeline"]) {
        c.pipeline.v4l2_nbuffers     = p["v4l2_nbuffers"    ].as<int>(c.pipeline.v4l2_nbuffers);
        c.pipeline.encoder_buffers   = p["encoder_buffers"  ].as<int>(c.pipeline.encoder_buffers);
        c.pipeline.queue_max_buffers = p["queue_max_buffers"].as<int>(c.pipeline.queue_max_buffers);
        c.pipeline.queue_leaky       = p["queue_leaky"      ].as<std::string>(c.pipeline.queue_leaky);
    }

    return c;
}

/* getopt_long 解析命令行；只覆盖被显式传入的字段 */
void Config::merge_cli(int argc, char** argv) {
    static const struct option opts[] = {
        {"config",     required_argument, nullptr, 'c'},
        {"device",     required_argument, nullptr, 'd'},
        {"port",       required_argument, nullptr, 'p'},
        {"bitrate",    required_argument, nullptr, 'b'},
        {"log-level",  required_argument, nullptr, 'l'},
        {"help",       no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };
    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "c:d:p:b:l:h", opts, nullptr)) != -1) {
        switch (opt) {
            case 'd': capture.device       = optarg; break;
            case 'p': server.port          = static_cast<uint16_t>(std::stoi(optarg)); break;
            case 'b': encoder.bitrate_kbps = std::stoi(optarg); break;
            case 'l': log.level            = optarg; break;
            case 'c': /* 在 main 中提前处理过 */ break;
            case 'h':
                std::printf(
                    "Usage: iotcam [--config FILE] [--device PATH] [--port N]\n"
                    "              [--bitrate KBPS] [--log-level LEVEL]\n");
                std::exit(0);
        }
    }
}