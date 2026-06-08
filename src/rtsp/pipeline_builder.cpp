//
// Created by vompom on on 2026/06/06 09:14.
//
// @Description
//

#include "pipeline_builder.h"


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
        // src_fmt = "NV12";
        // os << "v4l2h264enc extra-controls=\"controls"
        //    << ",h264_profile=4"
        //    << ",video_bitrate=" << (e.bitrate_kbps * 1000)
        //    << ",h264_i_frame_period=" << e.gop
        //    << ",h264_b_frames=" << e.bframes
        //    << "\"";
        // 使用dmabuf
        src_fmt = "NV12";
        os << "v4l2h264enc"
           << " capture-io-mode=4 output-io-mode=4"      // 双侧 dmabuf
           << " extra-controls=\"controls"
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

std::string downstream_for(const std::string& backend) {
    if (backend == "vaapi")   return "queue max-size-buffers=2 leaky=downstream";
    if (backend == "nvenc")   return "nvvideoconvert ! 'video/x-raw(memory:NVMM),format=NV12' ! "
                                     "queue max-size-buffers=2 leaky=downstream";
    if (backend == "v4l2m2m") return "queue max-size-buffers=2 leaky=downstream";
    return "videoconvert ! queue max-size-buffers=2 leaky=downstream"; // x264 软编兜底
}

std::string PipelineBuilder::build(const Config& c, Mode mode) {
    std::string src_fmt;
    std::string enc       = encoder_str(c.encoder, src_fmt);

    std::ostringstream os;

    if (mode == Mode::Dmabuf) {
        // ---------------- 真机零拷贝路径（暂未启用，detect_mode 永远 fallback 到 mmap）----------------
        // 保留代码骨架，等真机硬件调通 dmabuf 后再回来打磨。
        // 这里同样去掉 yaml 驱动的 filter_chain_str，避免历史遗留逻辑产生干扰。
        os << "( v4l2src device=" << c.capture.device
           << " do-timestamp=true io-mode=4"
           << " ! video/x-raw(memory:DMABuf),format=" << c.capture.pixfmt
           << ",width="     << c.capture.width
           << ",height="    << c.capture.height
           << ",framerate=" << c.capture.framerate << "/1"

           // 官方推荐写法：进入 GL 域之前先用 videoconvert 隔离 v4l2 池子
           << " ! videoconvert ! videoconvert"
           << " ! glupload ! glcolorconvert"
           << " ! gleffects effect=heat"
           << " ! glcolorconvert ! gldownload"
           << " ! videoconvert ! videoconvert"

           // M3 §T3.3：按编码器后端切换承接子串（统一 queue 出口）
           << " ! " << downstream_for(c.encoder.backend)

           // 编码器 + 打包
           << " ! " << enc
           << " ! h264parse config-interval=1"
           << " ! rtph264pay name=pay0 pt=96 mtu=1400 )";

    } else {
        // ---------------- mmap 降级路径（UTM / 无 dmabuf 真机）----------------
        //
        // 设计原则：
        //   1) 按 GStreamer 官方 GL tutorial 的最简形态搭建，不堆 caps-change-mode、
        //      不锁中间层 caps，不在 launch 字符串里写 GLSL；
        //   2) v4l2src 与 GL 元件之间用两层 videoconvert 隔开，让 GL 的
        //      propose_allocation 不会反向影响 v4l2 池子分配
        //      （之前 not-negotiated / "Uncertain or not enough buffers"
        //       / 单帧画面，根因都在这里）；
        //   3) 滤镜先用 GStreamer 自带的 gleffects effect=heat 验证 GL 通路稳定。
        //      通过后再切换为 glshader + 自定义 fragment（后续工作，单独提交）。
        //
        // 形态：
        //   v4l2src ! videoconvert ! videoconvert
        //          ! glupload ! glcolorconvert
        //          ! gleffects effect=heat
        //          ! glcolorconvert ! gldownload
        //          ! videoconvert ! videoconvert
        //          ! queue ! x264enc ! h264parse ! rtph264pay
        os << "( v4l2src device=" << c.capture.device << " do-timestamp=true"
           << " ! video/x-raw,format=" << c.capture.pixfmt
           << ",width="     << c.capture.width
           << ",height="    << c.capture.height
           << ",framerate=" << c.capture.framerate << "/1"

           // 官方推荐：两层 videoconvert 把 v4l2 与 GL 池子彻底隔开
           << " ! videoconvert ! videoconvert"

           // GL 段：上传 → 颜色转 → 滤镜（占位用 gleffects）→ 颜色转 → 下载
           // 不锁任何中间 caps，让 glcolorconvert 自己协商 RGBA / 尺寸
           << " ! glupload ! glcolorconvert"
           << " ! gleffects effect=heat"
           << " ! glcolorconvert ! gldownload"

           // 下 GL 后再两层 videoconvert，保证编码器吃到正确的格式
           << " ! videoconvert ! videoconvert"

           // 编码与打包
           << " ! queue max-size-buffers=2 leaky=downstream"
           << " ! " << enc
           << " ! h264parse config-interval=1"
           << " ! rtph264pay name=pay0 pt=96 mtu=1400 )";
    }
    return os.str();
}



static bool try_pipeline(const std::string& launch, int wait_ms = 1500) {
    GError* err = nullptr;
    GstElement* p = gst_parse_launch(launch.c_str(), &err);
    if (!p) {
        LOGW("parse_launch failed: {}", err ? err->message : "?");
        if (err) g_error_free(err);
        return false;
    }
    GstStateChangeReturn r = gst_element_set_state(p, GST_STATE_PAUSED);
    if (r == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
        return false;
    }
    /* 等一会儿看是否真的进入 PAUSED */
    GstState st;
    gst_element_get_state(p, &st, nullptr, wait_ms * GST_MSECOND);
    bool ok = (st == GST_STATE_PAUSED);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    return ok;
}

PipelineBuilder::Mode PipelineBuilder::detect_mode(const Config& c) {
    /* 先试 dmabuf */
    // auto launch_dma = build(c, Mode::Dmabuf);
    // if (try_pipeline(launch_dma)) {
    //     LOGI("dmabuf pipeline OK, using zero-copy path");
    //     return Mode::Dmabuf;
    // }
    // LOGW("dmabuf path failed, fallback to mmap+videoconvert");
    // todo:: dmabuf utm 目前还不支持，后面使用硬件再说
    return Mode::Mmap;
}
