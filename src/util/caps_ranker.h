//
// Created by vompom on 2026/06/13.
//
// @Description
//   CapsRanker：把 V4L2Prober 探测到的设备能力清单按"距离期望参数的远近"
//   排序，输出带评分的候选列表。上层 PipelineBuilder 拿第一个候选去拼 caps，
//   失败时按顺序回退到第二、第三个候选（启动期降级）。
//
//   评分模型（三项相加，分越低越优先）：
//     1. 格式权重：JPEG/YUY2/NV12/I420/其他 五档常量
//        - prefer_jpeg=true（默认）：JPEG 最优
//        - prefer_jpeg=false：把 JPEG 权重抬到与原始 raw 之后
//     2. 分辨率距离：|w-tw| + |h-th|
//     3. 帧率惩罚：|chosen_fps - target_fps| * kFpsPenaltyPerHz
//
//   纯函数实现：不读配置、不打日志、不依赖文件 IO，方便单元测试。
//

#ifndef VM_IOT_CAPS_RANKER_H
#define VM_IOT_CAPS_RANKER_H

#pragma once

#include "v4l2_prober.h"

#include <cstdint>
#include <string>
#include <vector>

namespace caps_ranker {

// 用户期望参数（从 yaml 配置映射而来）
struct Preference {
    uint32_t width  = 1280;
    uint32_t height = 720;
    double   fps    = 30.0;

    // true：JPEG 最优（USB 摄像头主流路径，码流小、走 jpegdec/jpegparse）
    // false：原始 raw 最优（要做 zero-copy GPU 上传时用）
    bool prefer_jpeg = true;
};

// 排序后的候选项
struct RankedCandidate {
    // 原始能力记录的 (fmt,w,h)，引用值拷贝，避免悬挂
    v4l2_prober::Capability cap;

    // 该候选下从 cap.fps_list 中挑出的、最接近 preference.fps 的那个 fps
    double chosen_fps = 0.0;

    // 评分（越小越优先），仅用于日志
    double score = 0.0;

    // 评分各项的明细，便于日志解释"为什么 A 比 B 排得靠前"
    double score_format     = 0.0;
    double score_resolution = 0.0;
    double score_fps        = 0.0;

    // 单行打印
    std::string to_string() const;
};

/**
 * 主排序入口。
 *
 * @param caps  来自 V4L2Prober::probe 的能力清单
 * @param pref  用户期望参数
 * @return      按 score 升序排序的候选列表（fps_list 为空的能力会被丢弃）
 */
std::vector<RankedCandidate> rank(
    const std::vector<v4l2_prober::Capability>& caps,
    const Preference& pref);

/**
 * 把排序结果格式化成多行日志，形如：
 *   [1] image/jpeg 1280x720 @60.0 score=4.0 (fmt=0 res=0 fps=4.0)
 *   [2] image/jpeg 1920x1080 @30.0 score=600.0 ...
 */
std::string format_ranking(const std::vector<RankedCandidate>& ranked);

// ============================================================================
// 以下纯函数仅为单元测试暴露
// ============================================================================

// 给定 media_type/raw_format 算出格式权重。规则在 .cpp 里集中维护。
double format_weight(const std::string& media_type,
                     const std::string& raw_format,
                     bool prefer_jpeg);

// 在 fps_list 中挑出最接近 target 的一个；空列表返回 0.0。
// 多个等距时优先选较大者（高帧率惯例上更优）。
double pick_closest_fps(const std::vector<double>& fps_list, double target);

} // namespace caps_ranker

#endif // VM_IOT_CAPS_RANKER_H
