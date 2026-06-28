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
#include <functional>
#include <unordered_map>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

/* ─────────── PagType ↔ string ───────────
 * 与 yaml "filter.pag.type" / pagfilter GObject "pag-type" 属性
 * 共用同一份字符串字面量，避免两边各写一份散开。 */
const char* to_string(PagType t) {
    switch (t) {
        case PagType::Sticker:   return "sticker";
        case PagType::PagEffect: return "pageffect";
    }
    return "sticker";
}

PagType pag_type_from_str(const std::string& s, PagType fallback) {
    /* 容忍大小写：yaml 写 "Sticker" / "STICKER" 都接受。 */
    std::string low; low.reserve(s.size());
    for (char c : s) low.push_back(static_cast<char>(::tolower(c)));
    if (low == "sticker")   return PagType::Sticker;
    if (low == "pageffect") return PagType::PagEffect;
    spdlog::warn("filter.pag.type='{}' unknown, fallback to '{}'",
                 s, to_string(fallback));
    return fallback;
}

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
        c.capture.device      = cap["device"     ].as<std::string>(c.capture.device);
        c.capture.width       = cap["width"      ].as<int>        (c.capture.width);
        c.capture.height      = cap["height"     ].as<int>        (c.capture.height);
        c.capture.framerate   = cap["framerate"  ].as<int>        (c.capture.framerate);
        c.capture.pixfmt      = cap["pixfmt"     ].as<std::string>(c.capture.pixfmt);
        c.capture.prefer_jpeg = cap["prefer_jpeg"].as<bool>       (c.capture.prefer_jpeg);
        c.capture.source      = cap["source"     ].as<std::string>(c.capture.source);

        /* source 合法性校验：只允许 auto | v4l2 | libcamera，未知值降级到 auto + warn。 */
        const std::string& src = c.capture.source;
        if (src != "auto" && src != "v4l2" && src != "libcamera") {
            spdlog::warn("capture.source='{}' unknown, fallback to 'auto' "
                         "(allowed: auto | v4l2 | libcamera)", src);
            c.capture.source = "auto";
        }
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
        c.filter.enabled      = f["enabled"    ].as<bool>       (c.filter.enabled);
        c.filter.shader       = f["shader"     ].as<std::string>(c.filter.shader);
        c.filter.filter_type  = f["filter_type"].as<int>        (c.filter.filter_type);
        c.filter.max_type     = f["max_type"   ].as<int>        (c.filter.max_type);

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

    /* filter.pag.*：读 enabled / selftest / type / file / position / scale。
         * file 为空且 enabled=true 允许：此时 pagfilter 仍在管线中，但会退化
         * 为 passthrough，行为等价于完全没有该元素。
         * selftest 与 enabled 独立：用来在不修改 pipeline 的前提下
         * 单独验证 libpag 链接是否通。
         * type 缺省 sticker；非法值 → fallback=Sticker + warn（在
         * pag_type_from_str 内部打印）。
         * position / scale 仅 type=Sticker 生效；越界值 clamp 后 warn。 */
    if (auto p = f["pag"]) {
        c.filter.pag.enabled  = p["enabled" ].as<bool>(c.filter.pag.enabled);
        c.filter.pag.selftest = p["selftest"].as<bool>(c.filter.pag.selftest);
        c.filter.pag.file     = p["file"    ].as<std::string>(c.filter.pag.file);
        if (p["type"]) {
            const std::string t = p["type"].as<std::string>("");
            c.filter.pag.type = pag_type_from_str(t, c.filter.pag.type);
        }
        if (auto pos = p["position"]) {
            c.filter.pag.pos_x = pos["x"].as<float>(c.filter.pag.pos_x);
            c.filter.pag.pos_y = pos["y"].as<float>(c.filter.pag.pos_y);
        }
        c.filter.pag.scale = p["scale"].as<float>(c.filter.pag.scale);

        /* clamp + warn：超出范围按上下限收敛，避免后续 blend / 渲染出现
         * 极端尺寸或 NaN。 */
        auto clamp_warn = [](float& v, float lo, float hi, const char* key) {
            if (!(v >= lo && v <= hi)) {                 // 同时拦 NaN（NaN 比较恒 false）
                spdlog::warn("filter.pag.{}={} out of [{}, {}], clamped",
                             key, v, lo, hi);
                v = std::max(lo, std::min(hi, std::isnan(v) ? lo : v));
            }
        };
        clamp_warn(c.filter.pag.pos_x, -2.0f, 3.0f,  "position.x");
        clamp_warn(c.filter.pag.pos_y, -2.0f, 3.0f,  "position.y");
        clamp_warn(c.filter.pag.scale,  0.01f, 8.0f, "scale");

        if (c.filter.pag.type == PagType::PagEffect) {
            spdlog::info("filter.pag.type=pageffect: position/scale ignored "
                         "in this mode (TODO: not implemented yet)");
        }
        }
    }

    if (auto ctl = n["control"]) {
        c.control.request_fifo = ctl["request_fifo"].as<std::string>(c.control.request_fifo);
        c.control.reply_fifo   = ctl["reply_fifo"  ].as<std::string>(c.control.reply_fifo);
    }

    if (auto s = n["snapshot"]) {
        c.snapshot.dir        = s["dir"       ].as<std::string>(c.snapshot.dir);
        c.snapshot.quality    = s["quality"   ].as<int>        (c.snapshot.quality);
        c.snapshot.timeout_ms = s["timeout_ms"].as<int>        (c.snapshot.timeout_ms);

        if (c.snapshot.quality < 1 || c.snapshot.quality > 100) {
            spdlog::warn("snapshot.quality={} out of [1,100], reset to 90", c.snapshot.quality);
            c.snapshot.quality = 90;
        }
        if (c.snapshot.timeout_ms <= 0) {
            spdlog::warn("snapshot.timeout_ms={} invalid, reset to 1500", c.snapshot.timeout_ms);
            c.snapshot.timeout_ms = 1500;
        }
    }

    // TODO(record): 录像功能暂未实现，YAML 中的 record 节点会被静默忽略。
    //               未来恢复时在此重新读取 record.enabled / dir / segment_sec / filename_pattern。

    return c;
}

/* ─────────────────────────── CLI 覆盖：setter 表 ───────────────────────────
 * 设计目标：
 *   1) 字段路径与 YAML 完全对齐（capture.width 在 yaml 即 capture: { width }），
 *      减少心智成本；新增配置只需在表里加 1 行。
 *   2) 快捷键（--port / --device / --bitrate / --log-level）保留向后兼容，
 *      内部转译为 setter 表的同一份逻辑，不出现两套。
 *   3) 未知 key、非法值 → 立即 exit，避免 typo 静默生效。
 */
namespace {

/* 解析助手：抛出 std::invalid_argument 让上层统一报错。 */
int parse_int(const std::string& v, const std::string& key) {
    try { return std::stoi(v); }
    catch (...) { throw std::invalid_argument("invalid int for '" + key + "': " + v); }
}
float parse_float(const std::string& v, const std::string& key) {
    try { return std::stof(v); }
    catch (...) { throw std::invalid_argument("invalid float for '" + key + "': " + v); }
}
uint16_t parse_u16(const std::string& v, const std::string& key) {
    int x = parse_int(v, key);
    if (x < 0 || x > 65535) throw std::invalid_argument("port out of range for '" + key + "': " + v);
    return static_cast<uint16_t>(x);
}
bool parse_bool(const std::string& v, const std::string& key) {
    std::string s; s.reserve(v.size());
    for (char c : v) s.push_back(static_cast<char>(::tolower(c)));
    if (s == "1" || s == "true"  || s == "yes" || s == "on")  return true;
    if (s == "0" || s == "false" || s == "no"  || s == "off") return false;
    throw std::invalid_argument("invalid bool for '" + key + "': " + v);
}

using Setter = std::function<void(Config&, const std::string&)>;

/* setter 表：唯一字段事实来源。 */
const std::unordered_map<std::string, Setter>& setters() {
    static const std::unordered_map<std::string, Setter> kMap = {
        {"server.port",          [](Config& c, const std::string& v){ c.server.port  = parse_u16(v, "server.port"); }},
        {"server.mount",         [](Config& c, const std::string& v){ c.server.mount = v; }},

        {"capture.device",       [](Config& c, const std::string& v){ c.capture.device    = v; }},
        {"capture.width",        [](Config& c, const std::string& v){ c.capture.width     = parse_int(v, "capture.width"); }},
        {"capture.height",       [](Config& c, const std::string& v){ c.capture.height    = parse_int(v, "capture.height"); }},
        {"capture.framerate",    [](Config& c, const std::string& v){ c.capture.framerate = parse_int(v, "capture.framerate"); }},
        {"capture.pixfmt",       [](Config& c, const std::string& v){ c.capture.pixfmt    = v; }},
        {"capture.prefer_jpeg",  [](Config& c, const std::string& v){ c.capture.prefer_jpeg = parse_bool(v, "capture.prefer_jpeg"); }},
        {"capture.source",       [](Config& c, const std::string& v){
            if (v != "auto" && v != "v4l2" && v != "libcamera")
                throw std::invalid_argument("invalid capture.source: " + v
                    + " (allowed: auto | v4l2 | libcamera)");
            c.capture.source = v;
        }},

        {"encoder.backend",      [](Config& c, const std::string& v){ c.encoder.backend      = v; }},
        {"encoder.bitrate_kbps", [](Config& c, const std::string& v){ c.encoder.bitrate_kbps = parse_int(v, "encoder.bitrate_kbps"); }},
        {"encoder.gop",          [](Config& c, const std::string& v){ c.encoder.gop          = parse_int(v, "encoder.gop"); }},
        {"encoder.bframes",      [](Config& c, const std::string& v){ c.encoder.bframes      = parse_int(v, "encoder.bframes"); }},

        {"log.level",            [](Config& c, const std::string& v){ c.log.level = v; }},

        {"filter.enabled",       [](Config& c, const std::string& v){ c.filter.enabled       = parse_bool(v, "filter.enabled"); }},
        {"filter.shader",        [](Config& c, const std::string& v){ c.filter.shader        = v; }},
        {"filter.filter_type",   [](Config& c, const std::string& v){ c.filter.filter_type   = parse_int(v, "filter.filter_type"); }},
        {"filter.max_type",      [](Config& c, const std::string& v){ c.filter.max_type      = parse_int(v, "filter.max_type"); }},

        {"filter.pag.enabled",     [](Config& c, const std::string& v){ c.filter.pag.enabled   = parse_bool(v, "filter.pag.enabled"); }},
        {"filter.pag.selftest",    [](Config& c, const std::string& v){ c.filter.pag.selftest  = parse_bool(v, "filter.pag.selftest"); }},
        {"filter.pag.type",        [](Config& c, const std::string& v){ c.filter.pag.type      = pag_type_from_str(v, c.filter.pag.type); }},
        {"filter.pag.file",        [](Config& c, const std::string& v){ c.filter.pag.file      = v; }},
        {"filter.pag.position.x",  [](Config& c, const std::string& v){ c.filter.pag.pos_x     = parse_float(v, "filter.pag.position.x"); }},
        {"filter.pag.position.y",  [](Config& c, const std::string& v){ c.filter.pag.pos_y     = parse_float(v, "filter.pag.position.y"); }},
        {"filter.pag.scale",       [](Config& c, const std::string& v){ c.filter.pag.scale     = parse_float(v, "filter.pag.scale"); }},

        {"control.request_fifo", [](Config& c, const std::string& v){ c.control.request_fifo = v; }},
        {"control.reply_fifo",   [](Config& c, const std::string& v){ c.control.reply_fifo   = v; }},

        {"snapshot.dir",         [](Config& c, const std::string& v){ c.snapshot.dir         = v; }},
        {"snapshot.quality",     [](Config& c, const std::string& v){ c.snapshot.quality     = parse_int(v, "snapshot.quality"); }},
        {"snapshot.timeout_ms",  [](Config& c, const std::string& v){ c.snapshot.timeout_ms  = parse_int(v, "snapshot.timeout_ms"); }},

        // TODO(record): 录像功能暂未实现，原 record.enabled / dir / segment_sec / filename_pattern
        //               setter 已从表中移除。未来重新接入时请同时恢复本表与 from_file 中的解析。
    };
    return kMap;
}

/* 应用一条 key=value，未知 key / 非法值都 exit(2)。 */
void apply_kv(Config& cfg, const std::string& key, const std::string& value) {
    const auto& m = setters();
    auto it = m.find(key);
    if (it == m.end()) {
        std::fprintf(stderr, "unknown config key: '%s'\n  available keys:\n", key.c_str());
        for (const auto& [k, _] : m) std::fprintf(stderr, "    %s\n", k.c_str());
        std::exit(2);
    }
    try {
        it->second(cfg, value);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "config error: %s\n", e.what());
        std::exit(2);
    }
}

void print_help() {
    std::printf(
        "Usage: iotcam [OPTIONS]\n"
"  -c, --config FILE         load YAML config (default: assets/config/default.yaml)\n"
        "  -d, --device PATH         alias of --set capture.device=PATH\n"
        "  -p, --port N              alias of --set server.port=N\n"
        "  -b, --bitrate KBPS        alias of --set encoder.bitrate_kbps=KBPS\n"
        "  -l, --log-level LEVEL     alias of --set log.level=LEVEL\n"
        "      --set KEY=VALUE       override any config field; KEY uses YAML dotted path\n"
        "  -h, --help                show this help\n"
        "\nAvailable --set keys:\n");
    for (const auto& [k, _] : setters()) std::printf("  %s\n", k.c_str());
}

} // namespace

/* getopt_long 解析命令行；只覆盖被显式传入的字段。
 * --set 与快捷键可任意混用，按出现顺序逐条应用，等价覆盖。 */
void Config::merge_cli(int argc, char** argv) {
    enum { OPT_SET = 1000 };
    static const struct option opts[] = {
        {"config",     required_argument, nullptr, 'c'},
        {"device",     required_argument, nullptr, 'd'},
        {"port",       required_argument, nullptr, 'p'},
        {"bitrate",    required_argument, nullptr, 'b'},
        {"log-level",  required_argument, nullptr, 'l'},
        {"set",        required_argument, nullptr, OPT_SET},
        {"help",       no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };
    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "c:d:p:b:l:h", opts, nullptr)) != -1) {
        switch (opt) {
            case 'd': apply_kv(*this, "capture.device",       optarg); break;
            case 'p': apply_kv(*this, "server.port",          optarg); break;
            case 'b': apply_kv(*this, "encoder.bitrate_kbps", optarg); break;
            case 'l': apply_kv(*this, "log.level",            optarg); break;
            case 'c': /* 在 main 中提前处理过 */ break;
            case OPT_SET: {
                std::string kv = optarg;
                auto eq = kv.find('=');
                if (eq == std::string::npos || eq == 0) {
                    std::fprintf(stderr, "--set expects KEY=VALUE, got '%s'\n", optarg);
                    std::exit(2);
                }
                apply_kv(*this, kv.substr(0, eq), kv.substr(eq + 1));
                break;
            }
            case 'h':
                print_help();
                std::exit(0);
            case '?':
                /* getopt_long 已经打印过错误信息 */
                std::exit(2);
        }
    }
}