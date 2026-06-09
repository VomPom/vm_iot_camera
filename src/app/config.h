//
// Created by vompom on on 2026/06/05 17:04.
//
// @Description
//   应用配置结构定义与 YAML 加载接口
//

#ifndef VM_IOT_CONFIG_H
#define VM_IOT_CONFIG_H

#include <string>
#include <cstdint>


struct ServerConfig {
    uint16_t port = 8554;
    std::string mount = "/live";
};

struct CaptureConfig {
    std::string device = "/dev/video0";
    int width = 1280;
    int height = 720;
    int framerate = 30;
    std::string pixfmt = "NV12";
};

struct EncoderConfig {
    std::string backend = "x264";
    int bitrate_kbps = 4000;
    int gop = 30;
    int bframes = 0;
};

struct LogConfig {
    std::string level = "info";
};

struct FilterConfig {
    bool        enabled    = true;
    std::string shader     = "effects.frag";         // 单一 shader 文件；运行时通过 uniform filter_type 切换分支
    int         filter_type = 0;                     // 启动默认特效：0=passthrough 1=mosaic 2=invert ...
    int         max_type    = 2;                     // filter next/prev 循环上限（含），与 shader 内 if 分支保持一致
    std::string control_fifo;                        // FIFO 控制通道路径，空串=不开启
};

struct Config {
    ServerConfig server;
    CaptureConfig capture;
    EncoderConfig encoder;
    LogConfig log;
    FilterConfig filter;

    std::string config_dir;

    static Config from_file(const std::string &path);

    void merge_cli(int argc, char **argv);
};

#endif //VM_IOT_CONFIG_H
