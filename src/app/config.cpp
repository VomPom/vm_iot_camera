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
#include <filesystem>
#include <spdlog/spdlog.h>

Config Config::from_file(const std::string& path) {
    Config c;
    YAML::Node n = YAML::LoadFile(path);

    try {
        c.config_dir = std::filesystem::absolute(path)
                            .parent_path().lexically_normal().string();
    } catch (...) {
        c.config_dir.clear();
    }

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

    if (auto f = n["filter"]) {
        c.filter.enabled      = f["enabled"     ].as<bool>       (c.filter.enabled);
        c.filter.shader       = f["shader"      ].as<std::string>(c.filter.shader);
        c.filter.filter_type  = f["filter_type" ].as<int>        (c.filter.filter_type);
        c.filter.max_type     = f["max_type"    ].as<int>        (c.filter.max_type);
        c.filter.control_fifo = f["control_fifo"].as<std::string>(c.filter.control_fifo);

        // 取值合法性校验
        if (c.filter.max_type < 0) {
            spdlog::warn("filter.max_type={} invalid, reset to 0", c.filter.max_type);
            c.filter.max_type = 0;
        }
        if (c.filter.filter_type < 0 || c.filter.filter_type > c.filter.max_type) {
            spdlog::warn("filter.filter_type={} out of [0,{}], reset to 0",
                         c.filter.filter_type, c.filter.max_type);
            c.filter.filter_type = 0;
        }
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