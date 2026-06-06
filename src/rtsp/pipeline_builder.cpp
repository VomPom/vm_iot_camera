//
// Created by vompom on on 2026/06/06 09:14.
//
// @Description
//

#include "pipeline_builder.h"

#include <sstream>
#include <stdexcept>


std::string PipelineBuilder::encoder_str(const EncoderConfig& e, std::string& src_fmt) {
    std::ostringstream os;
    if (e.backend == "vaapi") {
        src_fmt = "NV12";
        os << "vaapih264enc"
           << " rate-control=cbr"
           << " bitrate=" << e.bitrate_kbps
           << " keyframe-period=" << e.gop
           << " tune=low-power";
    } else if (e.backend == "nvenc") {
        src_fmt = "NV12";
        os << "nvh264enc"
           << " preset=low-latency-hp"
           << " rc-mode=cbr"
           << " bitrate=" << e.bitrate_kbps
           << " gop-size=" << e.gop;
    } else if (e.backend == "v4l2m2m") {
        src_fmt = "NV12";
        os << "v4l2h264enc extra-controls=\"controls"
           << ",h264_profile=4"
           << ",video_bitrate=" << (e.bitrate_kbps * 1000)
           << ",h264_i_frame_period=" << e.gop
           << ",h264_b_frames=" << e.bframes
           << "\"";
    } else if (e.backend == "x264") {
        src_fmt = "I420";
        os << "x264enc"
           << " tune=zerolatency"
           << " speed-preset=ultrafast"
           << " bitrate=" << e.bitrate_kbps
           << " key-int-max=" << e.gop
           << " bframes=" << e.bframes;
    } else {
        throw std::runtime_error("unknown encoder backend: " + e.backend);
    }
    return os.str();
}

std::string PipelineBuilder::build(const Config& c) {
    std::string src_fmt;
    std::string enc = encoder_str(c.encoder, src_fmt);

    std::ostringstream os;
    os << "( v4l2src device=" << c.capture.device << " do-timestamp=true"
       << " ! video/x-raw,format=" << c.capture.pixfmt
       << ",width="     << c.capture.width
       << ",height="    << c.capture.height
       << ",framerate=" << c.capture.framerate << "/1"
       << " ! videoconvert ! video/x-raw,format=" << src_fmt
       << " ! " << enc
       << " ! h264parse config-interval=1"
       << " ! rtph264pay name=pay0 pt=96 mtu=1400 )";
    return os.str();
}