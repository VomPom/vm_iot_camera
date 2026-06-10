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

std::string PipelineBuilder::build(const Config& c) {
    std::string src_fmt;
    std::string enc       = encoder_str(c.encoder, src_fmt);

    // 根据 backend 选择 parse + RTP payloader。
    // x265 走 H.265 链路（rtph265pay），其余两个走 H.264 链路（rtph264pay）。
    const bool is_h265 = (c.encoder.backend == "x265");
    const char* parser = is_h265 ? "h265parse" : "h264parse";
    const char* payer  = is_h265 ? "rtph265pay" : "rtph264pay";

    std::ostringstream os;

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

    os << " ! videoconvert ! video/x-raw,format=" << src_fmt
       << " ! videoconvert"

       // 编码与打包
       << " ! queue max-size-buffers=2 leaky=downstream"
       << " ! " << enc
       << " ! " << parser << " config-interval=1"
       << " ! " << payer  << " name=pay0 pt=96 mtu=1400 )";

    return os.str();
}