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

/* ────────────────────── 管道架构总览 ─────────────────────
 *
 *   build() 输出一条供 gst-rtsp-server 解析的 launch 字符串。
 *
 *   ★ 取流锚点（tee）命名约定 ★
 *     视频：
 *       - tee name=t        → 原始 raw（cfg.capture.pixfmt 已收敛），
 *                              任何需要"像素"的副线从这里拉（snapshot / detect / motion ...）。
 *       - tee name=enc_t    → 编码后 H.264/H.265 elementary stream，
 *                              任何需要"码流"的副线从这里拉（main rtp / record ...）。
 *     音频（cfg.audio.enabled=true 才开）：
 *       - tee name=at       → 原始 PCM（已 audioconvert/resample 收敛），未来为 ASR / level 预留。
 *       - tee name=enc_at   → 编码后音频 ES（AAC / Opus），供 RTP pay1 / record(TODO mp4mux) 拉。
 *     新增副线必须从四锚点中选一，禁止再起其他 tee。
 *
 *   整体结构（视频主链，音频与之并行）：
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
 *      videoconvert  ─►  [可选 GL 滤镜段]  ─►  videoconvert
 *                                                       │
 *                                                       ▼
 *                                                 tee  name=t  ◄── raw 锚点
 *                              ┌───────────────────┬────┴────┐
 *                              │                   │         │
 *                       (副线 snapshot)     (主线编码段)  (规划：detect / motion / ai)
 *                              │                   │
 *                              ▼                   ▼
 *                      jpeg 截图            videoconvert ! encoder ! parse
 *                                                   │
 *                                                   ▼
 *                                            tee  name=enc_t  ◄── 码流锚点
 *                                         ┌─────────┴─────────┐
 *                                         │                   │
 *                                  (主线 rtp pay)      (副线 record，TODO)
 *                                         │                   │
 *                                         ▼                   ▼
 *                                  rtph26Xpay name=pay0   暂不追加任何元素
 *
 *   音频主链（与视频平行，同一组 ( ... ) 内，cfg.audio.enabled=true 时才开）：
 *      alsasrc
 *         │ (caps: rate / channels)
 *         ▼
 *      audioconvert ! audioresample
 *         │
 *         ▼
 *      volume name=aud_vol
 *         │ (PLAYING 可热调)
 *         ▼
 *      valve name=aud_valve   (drop=mute, PLAYING 可热调)
 *         │
 *         ▼
 *      tee name=at  ◄── 原始 PCM 锚点
 *         │ at. ! queue ! audioconvert ! aac/opus 编码 ! aacparse?
 *         ▼
 *      tee name=enc_at  ◄── 音频码流锚点
 *         │ enc_at. ! queue ! rtpmp4apay/rtpopuspay name=pay1
 *         ▼
 *      RTSP pay1（另一路与 pay0 同机 SDP）
 *
 *   副线清单：
 *     ┌────────────┬──────────┬──────────────┬────────┬────────────────────────┐
 *     │ 副线        │ 锚点     │ 落点          │ 状态   │ queue 策略              │
 *     ├────────────┼──────────┼──────────────┼────────┼────────────────────────┤
 *     │ main(rtp)  │ enc_t.   │ rtph26Xpay   │ 已实现  │ leaky=downstream(2)     │
 *     │ snapshot   │ t.       │ jpegenc/file │ 已实现  │ leaky=downstream(2)     │
 *     │ audio(rtp) │ enc_at.  │ rtpmp4apay/  │ 已实现  │ leaky=no(200ms)         │
 *     │            │          │ rtpopuspay   │         │                          │
 *     │ record    │ enc_t.+  │ mp4mux+file  │ TODO    │ 原计划 no-leaky 大缓冲     │
 *     │           │ enc_at.  │             │         │                          │
 *     │ detect    │ t.       │ appsink     │ 规划中 │ leaky=downstream(2)     │
 *     │ motion    │ t.       │ msg/event   │ 规划中 │ leaky=downstream(2)     │
 *     └────────────┴──────────┴──────────────┴────────┴────────────────────────┘
 *
 *   分流原则（添加新副线时遵循）：
 *     1) 四锚点二选一：要像素去 t.，要码流去 enc_t.，要原始音频去 at.，要音频码流去 enc_at.；不再起新 tee。
 *     2) 副线开头必须有 queue：snapshot/detect 类用 leaky=downstream（可丢），
 *        record 类用 no-leaky 大缓冲（落盘不能丢帧）；音频主链 queue 用 200ms time-based。
 *     3) 副线首端放一个 `valve drop=true` 默认关闭，按需打开；
 *        主线音频上的 `aud_valve` 例外：默认 drop=false，作静音开关用。
 *     4) 副线内的元素命名遵循 `<branch>_<role>`（snap_valve / rec_tail / det_appsink）；
 *        音频主链统一 `aud_*` 前缀（aud_vol / aud_valve）。
 *     5) build() 内部按副线拆分成 append_branch_<x>() 函数。音频拆为
 *        build_audio_source_segment / append_audio_main_encode_segment /
 *        append_branch_audio_main_rtp 三个子函数。
 *     6) A/V 同步约束：音频路径不放 videorate 类重打 PTS 的元素；
 *        v4l2src 与 alsasrc 都开 do-timestamp=true，共用 pipeline clock。
 *
 * ────────────────────────────────────────────────────────────────── */

/* 主线编码段（公共上游 → tee name=enc_t）：把编码 + parse 提到 tee 之前，
 * 这样 record 副线可以零成本复用同一份 H.264/H.265 ES。
 * pay0 是 gst-rtsp-server 约定名，必须保留。 */
static void append_main_encode_segment(std::ostringstream& os,
                                       const std::string&  src_fmt,
                                       const std::string&  enc_str,
                                       bool                is_h265) {
    const char* parser = is_h265 ? "h265parse"  : "h264parse";

    os << " t. ! queue max-size-buffers=2 leaky=downstream"
       << " ! videoconvert ! video/x-raw,format=" << src_fmt
       << " ! " << enc_str
       << " ! " << parser << " config-interval=1"
       << " ! tee name=enc_t";
}

/* 主线 RTP 出口副线：从 enc_t. 拉 ES，打成 RTP。 */
static void append_branch_main_rtp(std::ostringstream& os, bool is_h265) {
    const char* payer  = is_h265 ? "rtph265pay" : "rtph264pay";
    os << " enc_t. ! queue max-size-buffers=2 leaky=downstream"
       << " ! " << payer << " name=pay0 pt=96 mtu=1400";
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

static void append_branch_record(std::ostringstream& /*os*/) {
    // 暂不在 launch 中追加任何录像副线元素；主线 RTSP 推流不受影响。
    // 保留函数名与调用点作为未来恢复的坐标。
}

// ============================================================================
// 音频主链（alsasrc → caps → audioconvert/resample → volume → valve →
//   tee at → encoder → aacparse? → tee enc_at → rtp pay1）
// ============================================================================

std::string PipelineBuilder::audio_encoder_str(const AudioEncoderConfig& e) {
    std::ostringstream os;
    if (e.backend == AudioEncoderBackend::AAC) {
        /* voaacenc / avenc_aac 都以 bps 为单位。 */
        const char* factory = (e.aac_impl == AudioAacImpl::VoAac) ? "voaacenc" : "avenc_aac";
        os << factory << " bitrate=" << (e.bitrate_kbps * 1000);
    } else { // Opus
        os << "opusenc bitrate=" << (e.bitrate_kbps * 1000);
    }
    return os.str();
}

std::string PipelineBuilder::build_audio_source_segment(const AudioConfig& a) {
    /* 仅 alsa 实现；pulse 由上层 from_str 中拦下 fallback 到 alsa。
     * device 为空串时不写 device= ，让 alsasrc 选系统默认。 */
    std::ostringstream os;
    os << "alsasrc";
    if (a.capture.do_timestamp) os << " do-timestamp=true";
    if (!a.capture.device.empty()) os << " device=" << a.capture.device;
    /* buffer-time / latency-time：微调起始拖带与周期，避免首 1~2 秒花屏/静默。
     * 200ms 总缓冲 / 20ms 周期是 USB mic 上验证过的保守值。 */
    os << " buffer-time=200000 latency-time=20000";
    os << " ! audio/x-raw,rate=" << a.capture.samplerate
       << ",channels="           << a.capture.channels;
    os << " ! audioconvert ! audioresample";
    return os.str();
}

/* 音频主线编码段（at. → encoder → aacparse?(opus 跳过) → tee enc_at）。
 * queue 用 200ms time-based + leaky=no：音频帧丢包会导致 AAC 帧不完整，客户端静默。
 * pay1 是 gst-rtsp-server 第二路媒体的约定名（与 pay0 对称）。 */
static void append_audio_main_encode_segment(std::ostringstream& os,
                                             const AudioEncoderConfig& e) {
    os << " at. ! queue max-size-time=200000000 max-size-buffers=0 max-size-bytes=0 leaky=no"
       << " ! audioconvert"
       << " ! " << PipelineBuilder::audio_encoder_str(e);
    if (e.backend == AudioEncoderBackend::AAC) {
        os << " ! aacparse";
    }
    os << " ! tee name=enc_at";
}

/* 音频 RTP 出口副线：enc_at. ! queue ! rtpmp4apay/rtpopuspay name=pay1 pt=97。 */
static void append_branch_audio_main_rtp(std::ostringstream& os,
                                         AudioEncoderBackend backend) {
    const char* payer = (backend == AudioEncoderBackend::AAC)
                            ? "rtpmp4apay"
                            : "rtpopuspay";
    os << " enc_at. ! queue max-size-time=200000000 max-size-buffers=0 max-size-bytes=0 leaky=no"
       << " ! " << payer << " name=pay1 pt=97 mtu=1400";
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
        LOGE("v4l2 probe returned empty for device={} (non-Linux or device unavailable)",
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
    os << " ! videoconvert";

    /* ── 可选 PAG 自研滤镜段 ──
     * 位置：GL 段之后、tee 之前，作用在 raw I420 系统内存上，
     * 与 GL 段解耦——任一段单独 enable 都能跑通。
     * name=pag0 是约定名，便于未来从 ControlChannel 通过
     * gst_bin_get_by_name 拿到实例做热控。
     * pagfilter 在 pag-file 非空时按 .pag 渲染并 alpha-blend 到画面上，
     * 为空则退化为 passthrough。 */
    if (c.filter.pag.enabled) {
        os << " ! pagfilter name=pag0";
    }

    os << " ! tee name=t";

    /* ── 分流副线 ── */
    /* 1) snapshot：从 raw tee 拉，jpeg 落盘（与编码段并行，零编码成本）。 */
    append_branch_snapshot(os);

    /* 2) 主线编码段：raw tee → encode → parse → tee name=enc_t（编码后的码流锚点）。 */
    append_main_encode_segment(os, src_fmt, enc, is_h265);

    /* 3) 主线 RTP：从 enc_t 拉 ES，打 RTP 给 gst-rtsp-server。 */
    append_branch_main_rtp(os, is_h265);

    /* 4) record（TODO）：录像副线暂未实现，调用保留作为未来恢复的锈点。 */
    append_branch_record(os);

#if VM_IOT_ENABLE_AUDIO
    /* ──── 音频主链（可选）────
     * 与视频主链在同一组 ( ... ) 内并行；cfg.audio.enabled=false 时本段不输出
     * 任何字符，与改造前的 launch 字符串 bit-for-bit 一致。
     * 拓扑：
     *   alsasrc ! caps ! audioconvert ! audioresample
     *           ! volume name=aud_vol ! valve name=aud_valve
     *           ! tee name=at
     *      at.     ! queue ! audioconvert ! encoder ! [aacparse] ! tee name=enc_at
     *      enc_at. ! queue ! rtpmp4apay/rtpopuspay name=pay1 */
    if (c.audio.enabled) {
        /* voaacenc 工厂探测：UTM aarch64 默认镜像有它，但有些精简镜像缺；
         * 缺了就 fallback 到 avenc_aac（gstreamer1.0-libav 提供）。
         * 探测必须放在 build() 里：单测里 VM_IOT_ENABLE_AUDIO=1 但不调 build()，
         * audio_encoder_str 仍是纯函数。 */
        AudioEncoderConfig eff_enc = c.audio.encoder;
        if (eff_enc.backend == AudioEncoderBackend::AAC &&
            eff_enc.aac_impl == AudioAacImpl::VoAac) {
            GstElementFactory* f = gst_element_factory_find("voaacenc");
            if (!f) {
                LOGW("audio: voaacenc factory not found, falling back to avenc_aac. "
                     "install gstreamer1.0-plugins-bad to enable voaacenc.");
                eff_enc.aac_impl = AudioAacImpl::AvEnc;
            } else {
                gst_object_unref(f);
            }
        }

        os << " " << build_audio_source_segment(c.audio);
        /* aud_vol：PLAYING 可热调，启动期从配置拍快照。
         * aud_valve：主线静音开关，与副线 valve(default drop=true) 语义不同。 */
        os << " ! volume name=aud_vol volume=" << c.audio.control.volume;
        os << " ! valve name=aud_valve drop=" << (c.audio.control.mute ? "true" : "false");
        os << " ! tee name=at";

        append_audio_main_encode_segment(os, eff_enc);
        append_branch_audio_main_rtp(os, eff_enc.backend);
    }
#endif

    os << " )";
    return os.str();
}