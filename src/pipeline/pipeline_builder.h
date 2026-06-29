//
// Created by vompom on on 2026/06/06 09:14.
//
// @Description
//   PipelineBuilder：根据 Config 拼装 gst-rtsp-server 的 launch 字符串。
//   接入 V4L2Prober + CapsRanker：上游 caps 由设备真实能力决定，
//   下游强制收敛到 cfg.capture.pixfmt，避免硬写死与设备不匹配的参数。
//

#ifndef VM_IOT_PIPELINE_BUILDER_H
#define VM_IOT_PIPELINE_BUILDER_H
#include <string>
#include "config.h"
#include "v4l2_prober.h"
#include <gst/gst.h>
#include "log.h"
#include <sstream>
#include <stdexcept>

class PipelineBuilder {
public:
    /**
     * 主入口：根据配置 + 设备真实能力生成 launch 字符串。
     *
     * 流程：
     *   1) V4L2Prober::probe() 拿到设备能力清单
     *   2) CapsRanker::rank()   按期望参数评分排序
     *   3) Top-1 候选 → 拼上游 caps + 解码（仅 jpeg 路径需要）
     *   4) 下游统一收敛到 cfg.capture.pixfmt（编码器输入）
     *
     * 兜底：
     *   - 探测失败（非 Linux / 设备打不开 / 无候选）→ 退化为"按配置硬拼"，
     *     与改造前行为一致；上游与下游 caps 都直接用 cfg.capture.*。
     *     这样 mac 端 build/集成、Linux 上设备临时拔除等场景仍可启动到
     *     有意义的报错点（GStreamer 自身会抛 negotiation 错误）。
     */
    static std::string build(const Config &c);

    // ========================================================================
    // 以下静态纯函数仅为单元测试暴露（不依赖 GStreamer 运行时、不读配置）
    // ========================================================================

    /**
     * 根据 ranker 选中的能力 + 实际选用的 fps，生成 v4l2src 之后第一段 caps。
     * 形如：
     *   image/jpeg,width=1280,height=720,framerate=30/1
     *   video/x-raw,format=YUY2,width=640,height=480,framerate=30/1
     *
     * fps 用一个分数化函数转成 num/denom（例如 29.97 → 30000/1001）。
     */
    static std::string make_input_caps(const v4l2_prober::Capability& cap,
                                       double fps);

    /**
     * 把浮点 fps 转成 GStreamer caps 用的 "num/denom" 字符串。
     *   30      → "30/1"
     *   29.97   → "30000/1001"
     *   59.94   → "60000/1001"
     *   120.10  → "120100/1000"   (一般够用的小数兜底)
     */
    static std::string fps_to_fraction(double fps);

    /**
     * 音频编码子串（仅拼 element + 必要属性，不拼上下游 ! 连接符）。
     *   AAC + voaacenc       → "voaacenc bitrate=96000"
     *   AAC + avenc_aac      → "avenc_aac bitrate=96000"
     *   Opus                  → "opusenc bitrate=64000"
     * voaacenc 缺失时调用方负责 fallback（build() 里完成）。 */
    static std::string audio_encoder_str(const AudioEncoderConfig& e);

    /**
     * 音频源段（v4l2src 的样子）：
     *   alsasrc do-timestamp=true device=hw:0,0
     *     ! audio/x-raw,rate=48000,channels=2
     *     ! audioconvert ! audioresample
     * 返回串以 "... ! " 结尾，供外层续接 volume / valve / tee。 */
    static std::string build_audio_source_segment(const AudioConfig& a);

private:
    /* 按 backend 返回编码器子串与所需输入像素格式 */
    static std::string encoder_str(const EncoderConfig &e, std::string &src_fmt);
};

#endif //VM_IOT_PIPELINE_BUILDER_H
