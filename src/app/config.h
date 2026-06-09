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


#pragma once
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

struct PipelineConfig {
    int v4l2_nbuffers = 4;
    int encoder_buffers = 4;
    int queue_max_buffers = 2;
    std::string queue_leaky = "downstream"; // upstream | downstream | no
};

struct FilterConfig {
    bool        enabled    = true;
    std::string shader     = "mosaic.frag";
};

struct Config {
    ServerConfig server;
    CaptureConfig capture;
    EncoderConfig encoder;
    LogConfig log;
    PipelineConfig pipeline;
    FilterConfig filter;

    std::string config_dir;

    static Config from_file(const std::string &path);

    void merge_cli(int argc, char **argv);
};

#endif //VM_IOT_CONFIG_H
