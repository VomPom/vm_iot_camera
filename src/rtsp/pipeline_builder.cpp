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

/* ─────────────────────────── 管道架构总览 ───────────────────────────
 *
 *   build() 输出一条供 gst-rtsp-server 解析的 launch 字符串，整体结构如下：
 *
 *      v4l2src  ──►  videoconvert  ──►  [可选 GL 滤镜段]  ──►  videoconvert
 *                                                                   │
 *                                                                   ▼
 *                                                              tee  name=t
 *                                                          ┌────────┴────────┐
 *                                                          │                 │
 *                                                  ┌───────┘                 └──────────┐
 *                                                  ▼                                    ▼
 *                                          (主线 main)                           (副线 snapshot)
 *                                       推流：编码 + RTP 打包                    截图：JPEG 单帧落盘
 *                                                  │                                    │
 *                                                  ▼                                    ▼
 *                                  encoder ─► parse ─► rtph26Xpay              valve ─► jpegenc ─► multifilesink
 *                                            (name=pay0)                        (name=snap_valve / snap_sink)
 *
 *   分流原则（添加新副线时遵循）：
 *     1) 所有副线统一从 tee  name=t  拉取，零拷贝、不影响主线。
 *     2) 每条副线开头必须有 `queue leaky=downstream max-size-buffers=2`：
 *        副线卡住时丢自己的帧，不反压主线推流。
 *     3) 每条副线首端放一个 `valve drop=true` 默认关闭：
 *        平时副线零开销，按需由 ControlChannel 打开 valve 触发动作。
 *     4) 副线内的元素命名遵循 `<branch>_<role>`（如 snap_valve / snap_sink），
 *        C++ 端通过 gst_bin_get_by_name() 抓取并控制。
 *     5) build() 内部按副线拆分成 append_branch_<x>() 函数，
 *        新增分流（录像 / AI 推理 / 运动检测）时只需追加一个 append 函数。
 *
 *   现有副线清单：
 *     - snapshot：JPEG 截图（默认 valve 关闭，iotcamctl snapshot 触发）
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
 *
 **/
static void append_branch_snapshot(std::ostringstream& os) {
    os << " t. ! queue max-size-buffers=2 leaky=downstream silent=true"
       << " ! valve name=snap_valve drop=true"
       << " ! videoconvert"
       << " ! jpegenc quality=90"
       << " ! multifilesink name=snap_sink"
       <<       " location=/tmp/vm_iot_snap_unused.jpg"
       <<       " post-messages=true async=false sync=false";
}

std::string PipelineBuilder::build(const Config& c) {
    std::string src_fmt;
    std::string enc       = encoder_str(c.encoder, src_fmt);
    const bool  is_h265   = (c.encoder.backend == "x265");

    std::ostringstream os;

    /* ── 采集 + 可选 GL 滤镜段，输出汇入 tee ── */
    os << "( v4l2src device=" << c.capture.device << " do-timestamp=true"
       << " ! video/x-raw,format=" << c.capture.pixfmt
       << ",width="     << c.capture.width
       << ",height="    << c.capture.height
       << ",framerate=" << c.capture.framerate << "/1"
       << " ! videoconvert ! videoconvert";

    // GL 滤镜段：仅当 filter.enabled 为 true 才插入。
    // 元素名 f0 是内部约定（rtsp_server 会按名查找并 g_object_set fragment + uniforms），
    // 对外只暴露 yaml 中的 filter.enabled / filter.shader / filter.filter_type / filter.max_type。
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