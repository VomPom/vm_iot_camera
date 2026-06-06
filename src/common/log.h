//
// Created by vompom on on 2026/06/06 09:16.
//
// @Description
//

#ifndef VM_IOT_LOG_H
#define VM_IOT_LOG_H

#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <string>

inline void setup_logger(const std::string &level) {
    auto logger = spdlog::stdout_color_mt("iotcam");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    if (level == "trace") spdlog::set_level(spdlog::level::trace);
    else if (level == "debug") spdlog::set_level(spdlog::level::debug);
    else if (level == "info") spdlog::set_level(spdlog::level::info);
    else if (level == "warn") spdlog::set_level(spdlog::level::warn);
    else if (level == "err") spdlog::set_level(spdlog::level::err);
    spdlog::flush_every(std::chrono::seconds(1));
}


#define LOGT(...) spdlog::trace(__VA_ARGS__)
#define LOGD(...) spdlog::debug(__VA_ARGS__)
#define LOGI(...) spdlog::info(__VA_ARGS__)
#define LOGW(...) spdlog::warn(__VA_ARGS__)
#define LOGE(...) spdlog::error(__VA_ARGS__)
#endif //VM_IOT_LOG_H
