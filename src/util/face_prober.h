//
// Created by vompom on 2026/06/30.
//
// @Description
//   Face 副线启动期前置检查：
//     1) gst-plugins-bad 中 opencv 子模块提供的 `facedetect` element
//        必须在当前 GStreamer 注册表里能找到（apt 包 gstreamer1.0-opencv）；
//     2) cfg.face.detect.cascade 必填且 stat 可读；
//     3) cfg.face.detect.profile/nose/mouth/eyes 非空时，每个都必须 stat 可读。
//
//   设计取舍：
//     - 与 v4l2_prober::device_accessible / alsa_prober::device_accessible
//       保持同款命名 + 同款 errno 风格，启动期失败 main 直接退出码 5。
//     - 不引入 OpenCV 头文件依赖；所有 OpenCV 计算在 facedetect 内部完成。
//     - 错误信息里附 apt 命令，运维拿到日志即可一步修复。
//

#ifndef VM_IOT_FACE_PROBER_H
#define VM_IOT_FACE_PROBER_H

#pragma once

#include <string>

#include "config.h"

namespace face_prober {

/**
 * 启动期检查 face 副线就绪性。
 *
 * @param cfg 完整 FaceConfig；仅在 cfg.enabled=true 时进行真实检查，否则直接 return true。
 * @param err 失败时填充人类可读错误信息（含 apt 命令提示），调用方直接打到 stderr。
 * @return true=就绪可挂副线；false=配置/运行环境不满足。
 */
bool is_ready(const FaceConfig& cfg, std::string* err);

} // namespace face_prober

#endif // VM_IOT_FACE_PROBER_H
