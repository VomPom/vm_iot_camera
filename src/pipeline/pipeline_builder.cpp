//
// Created by vompom on on 2026/06/06 09:14.
//
// @Description
//   软件编码后端拼装：仅支持在通用 Linux/UTM aarch64 上可用的三种纯软编：
//     - x264     (H.264, x264enc)        —— 默认；兼容性最好
//     - openh264 (H.264, openh264enc)    —— Cisco 软编，备选
//     - x265     (H.265, x265enc)        —— 码率更省，CPU 占用更高，注意客户端兼容性
//   不再支持 vaapi / nvenc / v4l2m2m，这三类后端在目标运行环境（UTM aarch64）上不可用。
//

#include "pipeline_builder.h"
#include "caps_ranker.h"

#include <cmath>
#include <cstdint>

std::string PipelineBuilder::encoder_str(const EncoderConfig& e, std::string& src_fmt) {
    std::ostringstream os;
    if (e.backend == "x264") {
        // x264enc 直接吃 I420，零拷贝最少绕路。
        src_fmt = "I420";
        os << "x264enc"
           << " tune=zerolatency"
           << " speed-preset=ultrafast"
           << " bitrate=" << e.bitrate_kbps
           << " key-int-max=" << e.gop
           << " bframes=" << e.bframes;
    } else if (e.backend == "openh264") {
        // openh264enc 同样吃 I420；不暴露 bframes（实现上不支持 B 帧）。
        src_fmt = "I420";
        os << "openh264enc"
           << " bitrate=" << (e.bitrate_kbps * 1000)   // openh264 用 bps
           << " gop-size=" << e.gop
           << " complexity=low"
           << " rate-control=bitrate";
    } else if (e.backend == "x265") {
        // x265enc 默认极慢，必须显式 ultrafast + zerolatency 才能在 UTM 上跑得动。
        // 不暴露 bframes（属性名与 x264 不一致，且默认行为已能满足低延迟场景）。
        src_fmt = "I420";
        os << "x265enc"
           << " tune=zerolatency"
           << " speed-preset=ultrafast"
           << " bitrate=" << e.bitrate_kbps
           << " key-int-max=" << e.gop;
    } else {
        throw std::runtime_error("unknown encoder backend: " + e.backend
                                 + " (supported: x264 | openh264 | x265)");
    }
    return os.str();
}

// ============================================================================
// 静态纯函数：caps 字符串构造（可单测）
// ============================================================================

std::string PipelineBuilder::fps_to_fraction(double fps) {
    if (fps <= 0.0) return "30/1";

    // 常用 NTSC 分数帧率特判，落到精确分数避免 caps 协商飘移
    constexpr double kEps = 1e-2;
    if (std::fabs(fps - 29.97) < kEps) return "30000/1001";
    if (std::fabs(fps - 59.94) < kEps) return "60000/1001";
    if (std::fabs(fps - 23.976) < kEps) return "24000/1001";

    // 整数 fps：直接 N/1
    double rounded = std::round(fps);
    if (std::fabs(fps - rounded) < 1e-3) {
        std::ostringstream os;
        os << static_cast<int64_t>(rounded) << "/1";
        return os.str();
    }

    // 其他小数 fps：按 1000 倍数做兜底（120.10 → 120100/1000）
    std::ostringstream os;
    os << static_cast<int64_t>(std::round(fps * 1000.0)) << "/1000";
    return os.str();
}

std::string PipelineBuilder::make_input_caps(const v4l2_prober::Capability& cap,
                                             double fps) {
    std::ostringstream os;
    os << cap.media_type;
    if (cap.media_type == "video/x-raw" && !cap.raw_format.empty()) {
        os << ",format=" << cap.raw_format;
    }
    os << ",width="     << cap.width
       << ",height="    << cap.height
       << ",framerate=" << fps_to_fraction(fps);
    return os.str();
}

/* ─────────────────────────── 管道架构总览 ───────────────────────────
 *
 *   build() 输出一条供 gst-rtsp-server 解析的 launch 字符串，整体结构如下：
 *
 *      v4l2src
 *         │ (上游 caps 由 V4L2Prober + CapsRanker 决定，jpeg 或 raw 二选一)
 *         ▼
 *      [仅 jpeg 路径]  jpegparse ! jpegdec
 *         │
 *         ▼
 *      videoconvert ! videoscale ! videorate
 *         │ (下游统一 caps：format=cfg.capture.pixfmt + 期望分辨率/帧率)
 *         ▼
 *      videoconvert  ──►  [可选 GL 滤镜段]  ──►  videoconvert
 *                                                       │
 *                                                       ▼
 *                                                 tee  name=t
 *                                             ┌────────┴────────┐
 *                                             │                 │
 *                                     ┌───────┘                 └──────────┐
 *                                     ▼                                    ▼
 *                              (主线 main)                          (副线 snapshot)
 *                            推流：编码 + RTP 打包                  截图：JPEG 单帧落盘
 *                                     │                                    │
 *                                     ▼                                    ▼
 *                  encoder ─► parse ─► rtph26Xpay (name=pay0)    valve ─► jpegenc ─► multifilesink
 *                                                                (name=snap_valve / snap_sink)
 *
 *   分流原则（添加新副线时遵循）：
 *     1) 所有副线统一从 tee  name=t  拉取，零拷贝、不影响主线。
 *     2) 每条副线开头必须有 `queue leaky=downstream max-size-buffers=2`。
 *     3) 每条副线首端放一个 `valve drop=true` 默认关闭，按需打开。
 *     4) 副线内的元素命名遵循 `<branch>_<role>`（如 snap_valve / snap_sink）。
 *     5) build() 内部按副线拆分成 append_branch_<x>() 函数。
 *
 *   现有副线清单：
 *     - snapshot：JPEG 截图
 *   规划副线：
 *     - record  ：分段 MP4 录像  (mp4mux ! filesink，valve 控制起停)
 *     - ai      ：appsink 喂 C++ 推理（人脸/物体检测）
 *     - motion  ：videoanalyse / motioncells，事件走 bus message
 *
 * ──────────────────────────────────────────────────────────────────── */

/* 主线（必有）：tee. ! queue ! convert ! 编码 ! parse ! rtp pay
 * pay0 是 gst-rtsp-server 约定名，必须保留。 */
static void append_branch_main(std::ostringstream& os,
                               const std::string&  src_fmt,
                               const std::string&  enc_str,
                               bool                is_h265) {
    const char* parser = is_h265 ? "h265parse"  : "h264parse";
    const char* payer  = is_h265 ? "rtph265pay" : "rtph264pay";

    os << " t. ! queue max-size-buffers=2 leaky=downstream"
       << " ! videoconvert ! video/x-raw,format=" << src_fmt
       << " ! " << enc_str
       << " ! " << parser << " config-interval=1"
       << " ! " << payer  << " name=pay0 pt=96 mtu=1400";
}

/**
 * 截图副线：tee. ! queue ! valve(默认关) ! convert ! jpegenc ! multifilesink
 * 运行时由 Snapshot 模块按 name=snap_valve / snap_sink 抓取并控制。
 * post-messages=true 让 sink 写完每个文件后发 element message，便于上层确认完成。
 */
static void append_branch_snapshot(std::ostringstream& os) {
    os << " t. ! queue max-size-buffers=2 leaky=downstream silent=true"
       << " ! valve name=snap_valve drop=true"
       << " ! videoconvert"
       << " ! jpegenc quality=90"
       << " ! multifilesink name=snap_sink"
       <<       " location=/tmp/vm_iot_snap_unused.jpg"
       <<       " post-messages=true async=false sync=false";
}

// ============================================================================
// 上游 source 段：探测 + 排序 + 兜底
// ============================================================================

namespace {

/**
 * 探测设备 + 排序，返回选中的 (Capability, fps)。
 * 探测失败或排序后无候选时，返回 {false, ...}，由调用方走兜底。
 */
struct ChosenInput {
    bool ok = false;
    v4l2_prober::Capability cap;
    double fps = 30.0;
};

ChosenInput choose_input(const Config& c) {
    auto caps = v4l2_prober::probe(c.capture.device);
    if (caps.empty()) {
        LOGW("v4l2 probe returned empty for device={} (non-Linux or device unavailable)",
             c.capture.device);
        return {};
    }
    LOGI("v4l2 capabilities for {}:\n{}",
         c.capture.device, v4l2_prober::format_table(caps));

    caps_ranker::Preference pref;
    pref.width       = static_cast<uint32_t>(c.capture.width);
    pref.height      = static_cast<uint32_t>(c.capture.height);
    pref.fps         = static_cast<double>(c.capture.framerate);
    pref.prefer_jpeg = c.capture.prefer_jpeg;

    auto ranked = caps_ranker::rank(caps, pref);
    if (ranked.empty()) {
        LOGW("caps_ranker produced no candidates");
        return {};
    }
    LOGI("caps ranking (top {} of {}):\n{}",
         std::min<size_t>(ranked.size(), 5),
         ranked.size(),
         caps_ranker::format_ranking(
             {ranked.begin(),
              ranked.begin() + std::min<size_t>(ranked.size(), 5)}));

    ChosenInput out;
    out.ok  = true;
    out.cap = ranked.front().cap;
    out.fps = ranked.front().chosen_fps;
    LOGI("selected input: {} @ {:.1f} fps (score={:.1f})",
         out.cap.to_string(), out.fps, ranked.front().score);
    return out;
}

/**
 * 上游 source 段拼装：v4l2src + caps + 可选 jpeg 解码。
 * 返回的串以"... ! "结尾，外层继续拼下游 videoconvert/scale/rate。
 */
std::string build_source_segment(const Config& c) {
    std::ostringstream os;
    os << "v4l2src device=" << c.capture.device << " do-timestamp=true ! ";

    auto chosen = choose_input(c);
    if (chosen.ok) {
        os << PipelineBuilder::make_input_caps(chosen.cap, chosen.fps) << " ! ";
        if (chosen.cap.media_type == "image/jpeg") {
            // jpegparse 容错性比 jpegdec 直接吃要好（处理偶发坏帧）
            os << "jpegparse ! jpegdec ! ";
        }
    } else {
        // 兜底：完全按用户配置硬拼，与改造前一致；启动失败时 GStreamer 会自己报错
        LOGW("falling back to hard-coded caps (legacy behavior): "
             "format={} {}x{}@{}",
             c.capture.pixfmt, c.capture.width, c.capture.height, c.capture.framerate);
        os << "video/x-raw,format=" << c.capture.pixfmt
           << ",width="     << c.capture.width
           << ",height="    << c.capture.height
           << ",framerate=" << c.capture.framerate << "/1 ! ";
    }
    return os.str();
}

} // namespace

// ============================================================================
// build()
// ============================================================================

std::string PipelineBuilder::build(const Config& c) {
    std::string src_fmt;
    std::string enc       = encoder_str(c.encoder, src_fmt);
    const bool  is_h265   = (c.encoder.backend == "x265");

    std::ostringstream os;

    /* ── 上游：v4l2src + 探测得到的 caps + 解码（如需） ── */
    os << "( " << build_source_segment(c);

    /* ── 下游统一 caps：把任意上游格式收敛成 cfg.capture.pixfmt + 期望分辨率/帧率 ──
     * videoscale / videorate 加上是为了应对 ranker 选到的尺寸/帧率 ≠ 期望值的情况
     * （能力清单里没有 1280x720@30 时会退而求其次选邻居，这里再缩放/重采样回标准） */
    os << "videoconvert ! videoscale ! videorate"
       << " ! video/x-raw,format=" << c.capture.pixfmt
       << ",width="     << c.capture.width
       << ",height="    << c.capture.height
       << ",framerate=" << c.capture.framerate << "/1";

    /* ── 可选 GL 滤镜段 ── */
    os << " ! videoconvert";
    if (c.filter.enabled) {
        os << " ! glupload ! glcolorconvert"
           << " ! glshader name=f0"
           << " ! glcolorconvert ! gldownload";
    }
    os << " ! videoconvert ! tee name=t";

    /* ── 分流副线 ── */
    append_branch_main(os, src_fmt, enc, is_h265);
    append_branch_snapshot(os);

    os << " )";
    return os.str();
}