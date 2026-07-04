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
 *   build() 输出一条供 gst-rtsp-server 解析的 launch 字符串。
 *
 *   ★ 取流锚点（tee）命名约定 ★
 *     - tee name=t      → 原始 raw（cfg.capture.pixfmt 已收敛），
 *                         任何需要"像素"的副线从这里拉（snapshot / detect / motion ...）。
 *     - tee name=enc_t  → 编码后 H.264/H.265 elementary stream，
 *                         任何需要"码流"的副线从这里拉（main rtp / record ...）。
 *     新增副线必须二选一锚点，禁止再起其他 tee。
 *
 *   整体结构：
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
 *                                                                  ↑
 *                                                  录像功能暂未实现，未来在这里
 *                                                  重新接 mp4mux+filesink 子 bin
 *
 *   副线清单：
 *     ┌────────────┬──────────┬──────────────┬────────┬────────────────────────┐
 *     │ 副线        │ 锈点     │ 落点          │ 状态   │ queue 策略              │
 *     ├────────────┼──────────┼──────────────┼────────┼────────────────────────┤
 *     │ main(rtp)  │ enc_t.   │ rtph26Xpay   │ 已实现  │ leaky=downstream(2)     │
 *     │ snapshot   │ t.       │ jpegenc/file │ 已实现  │ leaky=downstream(2)     │
 *     │ record     │ enc_t.   │ mp4mux+file  │ TODO    │ 原计划 no-leaky 大缓冲     │
 *     │ face       │ t.       │ facedetect+  │ 已实现  │ leaky=downstream(2)     │
 *     │            │          │ appsink      │        │ (cfg.face.enabled=true) │
 *     │ face_prev  │ t.       │ facedetect+  │ 可选    │ leaky=downstream(2)     │
 *     │            │          │ jpegenc+sink │        │ (preview_jpeg.enabled)  │
 *     │ motion     │ t.       │ msg/event    │ 规划中  │ leaky=downstream(2)     │
 *     └────────────┴──────────┴──────────────┴────────┴────────────────────────┘
 *
 *   分流原则（添加新副线时遵循）：
 *     1) 二选一锚点：要像素去 t.，要码流去 enc_t.；不再起新 tee。
 *     2) 副线开头必须有 queue：snapshot/detect 类用 leaky=downstream（可丢），
 *        record 类用 no-leaky 大缓冲（落盘不能丢帧）。
 *     3) 副线首端放一个 `valve drop=true` 默认关闭，按需打开。
 *     4) 副线内的元素命名遵循 `<branch>_<role>`（snap_valve / rec_tail / det_appsink）。
 *     5) build() 内部按副线拆分成 append_branch_<x>() 函数。
 *
 * ──────────────────────────────────────────────────────────────────── */

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

/* face 主检测副线：从 raw 锰点 t. 拉 → valve → videorate(节流) → RGB → facedetect → appsink。
 * - facedetect 输出不走 buffer payload，而是以 GST_MESSAGE_ELEMENT 投在 pipeline bus 上，
 *   由 FaceBranch 注册的 bus watch 解析。
 * - appsink max-buffers=2 drop=true sync=false：作为“流终结点”防止 pipeline 堡死，
 *   FaceBranch 不接 new-sample 信号（坐标全走 bus）。
 * - profile/nose/mouth/eyes 为空时不注入对应属性，以免 facedetect 启动期
 *   对空路径报错。 */
static void append_branch_face(std::ostringstream& os, const FaceConfig& cfg) {
    os << " t. ! queue max-size-buffers=2 leaky=downstream silent=true"
       << " ! valve name=face_valve drop="
       << (cfg.control.enabled_at_start ? "false" : "true");

    if (cfg.rate.fps_limit > 0) {
        os << " ! videorate"
           << " ! video/x-raw,framerate=" << cfg.rate.fps_limit << "/1";
    }

    os << " ! videoconvert ! video/x-raw,format=RGB"
       << " ! facedetect name=face0 display=false";

    /* facedetect 必填属性：profile-location / nose-location / ... 为 OpenCV plugin 约定名。
     * 名称与 gst-inspect-1.0 facedetect 输出一致。 */
    os << " profile=\"" << cfg.detect.cascade << "\"";
    if (!cfg.detect.profile.empty()) os << " profile-location=\"" << cfg.detect.profile << "\"";
    if (!cfg.detect.nose.empty())    os << " nose-location=\""    << cfg.detect.nose    << "\"";
    if (!cfg.detect.mouth.empty())   os << " mouth-location=\""   << cfg.detect.mouth   << "\"";
    if (!cfg.detect.eyes.empty())    os << " eyes-location=\""    << cfg.detect.eyes    << "\"";

    os << " min-size-width="  << cfg.detect.min_size_px
       << " min-size-height=" << cfg.detect.min_size_px
       << " scale-factor="    << cfg.detect.scale_factor
       << " min-neighbors="   << cfg.detect.min_neighbors;

    /* 主检测坐标全走 pipeline bus（GST_MESSAGE_ELEMENT），
     * 这里的终结点仅作"数据流出口"防止 pipeline 卡死。
     * 曾用 appsink max-buffers=2 drop=true：因 FaceBranch 未挂 new-sample
     * 也不主动 pull-sample，appsink 内部 queue 塞满 2 条后停止让位，
     * 上游 videorate/facedetect 被 back-pressure 卡住，导致主线只吐 1 条
     * facedetect message 就再无输出（preview 副线因尾部 valve drop=true
     * 就地丢字节，不产生 back-pressure，仍可正常出 message）。
     * 换成 fakesink async=false sync=false silent=true：不做时钟同步、
     * 收到就扔、不打印，绝无 back-pressure。element name 保持
     * "face_appsink"，与 FaceBranch::required_elements() 契约保持一致
     * （该名字只用作 gst_bin_get_by_name 查询，与 factory 类型解耦）。 */
    os << " ! fakesink name=face_appsink async=false sync=false silent=true";
}

/* face 画框预览副线（可选）：与主检测副线并列挂在 t. 上。
 * - 主动以 display=true 让 facedetect 在画面上黄框标记人脸。
 * - face_prev_valve 默认 drop=true，仅在 ControlChannel “face preview on” 后才出 JPEG。
 * - face_jpeg_sink emit-signals=false：本期只负责拼进 launch 串，未来接 HTTP
 *   端点时再在 FaceBranch 里改 emit-signals=true 并接 new-sample。 */
static void append_branch_face_preview(std::ostringstream&          os,
                                       const FacePreviewJpegConfig& p,
                                       const FaceDetectConfig&      d) {
    os << " t. ! queue max-size-buffers=2 leaky=downstream silent=true";
    if (p.fps_limit > 0) {
        os << " ! videorate"
           << " ! video/x-raw,framerate=" << p.fps_limit << "/1";
    }
    os << " ! videoconvert ! video/x-raw,format=RGB"
       << " ! facedetect display=true"
       << " profile=\"" << d.cascade << "\""
       << " min-size-width="  << d.min_size_px
       << " min-size-height=" << d.min_size_px
       << " scale-factor="    << d.scale_factor
       << " min-neighbors="   << d.min_neighbors
       << " ! videoconvert"
       << " ! jpegenc quality=" << p.jpeg_quality
       << " ! valve name=face_prev_valve drop=true"
       << " ! appsink name=face_jpeg_sink emit-signals=false"
       <<        " max-buffers=1 drop=true sync=false";
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

    /* 4) record（TODO）：录像副线暂未实现，调用保留作为未来恢复的锰点。 */
    append_branch_record(os);

    /* 5) face 人脸检测副线（可选）：从 raw tee=t 拉，主线 RTSP 零侵入。
     *    检测结果走 pipeline bus 以 GST_MESSAGE_ELEMENT 投递，由 FaceBranch 解析。
     *    preview_jpeg.enabled=true 时额外挂一条画框 JPEG 预览副线。 */
#if VM_IOT_ENABLE_FACEDETECT
    if (c.face.enabled) {
        append_branch_face(os, c.face);
        if (c.face.preview_jpeg.enabled) {
            append_branch_face_preview(os, c.face.preview_jpeg, c.face.detect);
        }
    }
#endif

    os << " )";
    return os.str();
}