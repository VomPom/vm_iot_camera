//
// Created by vompom on 2026/06/30.
//
// @Description
//   见 face_branch.h 顶部说明。
//
//   facedetect bus message 结构（来自 gst-plugins-bad opencv 子模块）：
//     name        = "facedetect"
//     timestamp   = guint64
//     stream-time / running-time / duration ...
//     faces       = (GValueArray of GstStructure {
//                       name="face", x=int, y=int, width=int, height=int })
//
//   解析点：g_value_get_boxed (GValueArray 在新版本 GStreamer 改为
//   GST_TYPE_LIST/GST_TYPE_ARRAY，统一走 gst_value_list_get_size+get_value 兜底)。
//

#include "face_branch.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "log.h"

void FaceBranch::configure(const FaceConfig& cfg) {
    cfg_ = cfg;
    LOGI("face_branch: configured enabled={} cascade='{}' fps_limit={} "
         "min_size_px={} cooldown_ms={} emit_when_empty={}",
         cfg_.enabled, cfg_.detect.cascade, cfg_.rate.fps_limit,
         cfg_.detect.min_size_px, cfg_.control.cooldown_ms,
         cfg_.control.emit_when_empty);
}

void FaceBranch::set_on_faces_callback(OnFacesCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    on_faces_cb_ = std::move(cb);
}

void FaceBranch::set_frame_size(int w, int h) {
    std::lock_guard<std::mutex> lk(mu_);
    frame_w_ = w;
    frame_h_ = h;
}

bool FaceBranch::on_attached_locked() {
    /* 三件套已由 BranchBase 抓到 elements_。本 branch 不再自己订阅 bus：
     * RtspServer 使用 gst_bus_enable_sync_message_emission + "sync-message" signal，在
     * streaming 线程将每条消息同步广播给每个 branch 的 on_bus_message_sync。好处：
     *   1) 完全绕开 GMainContext / thread-default 陷阱（之前的 add_signal_watch
     *      要么拿不到主 loop context、要么与已 acquire 的主 context 冲突）；
     *   2) 不占用 bus 的 sync handler slot，rtsp-server / bin 内部 preroll 行为
     *      保持不变（避免了 set_sync_handler 版本下“消息到了但推不出画面”
     *      的副作用）；
     *   3) sync-message signal 在 streaming 线程同步 emit，只要处理不重不阻塞，
     *      时延可忽略。 */
    GstElement* pipeline = pipeline_locked();
    if (!pipeline) {
        LOGW("face_branch: pipeline not available in on_attached_locked");
        return false;
    }

    /* enabled_at_start=false 时，face_valve.drop 已在 launch 字符串中按配置写好；
     * 这里无需再 set 一遍。保留 LOGI 把当前真实状态打印出来，便于排错。 */
    GstElement* fv = element("face_valve");
    gboolean cur_drop = TRUE;
    if (fv) g_object_get(fv, "drop", &cur_drop, nullptr);
    LOGI("face_branch: attached, face_valve.drop={}",
         cur_drop ? "true" : "false");
    last_state_   = {};
    last_emit_ts_ = {};
    last_log_ts_  = {};
    return true;
}

void FaceBranch::on_detaching_locked() {
    /* 本 branch 不再自己持有 bus 与 watch（sync handler 由 RtspServer
     * 统一在 media-unprepared 时清除）；required 元素 unref 由基类负责。 */
}

/* ─────────────────────── 控制 API ─────────────────────── */

bool FaceBranch::set_enabled(bool on, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* fv = element("face_valve");
    if (!fv) {
        if (err) *err = "not_attached";
        return false;
    }
    g_object_set(fv, "drop", on ? FALSE : TRUE, nullptr);
    LOGI("face_branch: set_enabled={}", on);
    return true;
}

bool FaceBranch::set_min_size(int px, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* fd = element("face0");
    if (!fd) {
        if (err) *err = "not_attached";
        return false;
    }
    int clamped = std::max(24, std::min(1024, px));
    if (clamped != px && err) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "clamped_to_%d", clamped);
        *err = buf;
    }
    g_object_set(fd,
                 "min-size-width",  clamped,
                 "min-size-height", clamped,
                 nullptr);
    LOGI("face_branch: set_min_size px={} (input={})", clamped, px);
    return true;
}

void FaceBranch::format_status(std::string& out) const {
    std::lock_guard<std::mutex> lk(mu_);

    out.append("face_attached=").append(element("face_valve") ? "true" : "false").append("\n");

    GstElement* fv = element("face_valve");
    gboolean cur_drop = TRUE;
    if (fv) g_object_get(fv, "drop", &cur_drop, nullptr);
    out.append("face_enabled=").append(cur_drop ? "false" : "true").append("\n");

    out.append("face_cascade=").append(cfg_.detect.cascade).append("\n");
    out.append("face_fps_limit=").append(std::to_string(cfg_.rate.fps_limit)).append("\n");

    GstElement* fd = element("face0");
    int min_w = cfg_.detect.min_size_px;
    if (fd) g_object_get(fd, "min-size-width", &min_w, nullptr);
    out.append("face_min_size_px=").append(std::to_string(min_w)).append("\n");

    out.append("face_count=").append(std::to_string(last_state_.count)).append("\n");

    /* 仅打前 4 个矩形避免日志过长。 */
    int dump_n = std::min<int>(4, static_cast<int>(last_state_.rects.size()));
    for (int i = 0; i < dump_n; ++i) {
        const auto& r = last_state_.rects[i];
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "face_rect%d=%d,%d %dx%d\n", i, r.x, r.y, r.w, r.h);
        out.append(buf);
    }

    if (last_emit_ts_.time_since_epoch().count() != 0) {
        auto now = std::chrono::steady_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_emit_ts_).count();
        out.append("face_last_event_ms_ago=").append(std::to_string(ms)).append("\n");
    } else {
        out.append("face_last_event_ms_ago=-1\n");
    }
}

/* ───────────────────── bus message ───────────────────── */

void FaceBranch::on_bus_message_sync(GstMessage* msg) {
    on_bus_message(msg);
}

void FaceBranch::on_bus_message(GstMessage* msg) {
    /* [HUNT-INSTR] 无条件 1Hz 计数器：证明 signal watch 是否真的收到消息。
     * 只统计 ELEMENT / ERROR / STATE_CHANGED / STREAM_START 四类，其它
     * QoS/位置流类忽略避免刷屏。1Hz 打一条汇总，见到即代表 bus 通路 ok。 */
    {
        auto now_dbg = std::chrono::steady_clock::now();
        GstMessageType t = GST_MESSAGE_TYPE(msg);
        const gchar* src_name = GST_MESSAGE_SRC(msg) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)) : "?";
        std::lock_guard<std::mutex> lk_dbg(mu_);
        dbg_msg_total_++;
        if (t == GST_MESSAGE_ELEMENT) {
            const GstStructure* ss = gst_message_get_structure(msg);
            const gchar* sn = ss ? gst_structure_get_name(ss) : "?";
            dbg_element_msg_total_++;
            if (sn && std::string(sn) == "facedetect") {
                dbg_facedetect_msg_total_++;
                /* 分开统计 face0（主检测）与 preview 副线（匿名 facedetect0）
                 * 各自的 message 数，证明主线到底有没有在推 message。 */
                if (src_name && std::string(src_name) == "face0") {
                    dbg_face0_msg_total_++;
                } else {
                    dbg_face_prev_msg_total_++;
                }
            }
            if (now_dbg - dbg_last_log_ - std::chrono::seconds(1) >= std::chrono::seconds(0)) {
                dbg_last_log_ = now_dbg;
                LOGI("face_branch[HUNT]: bus msg total={} element={} facedetect={} face0={} prev={} last_element={} src={}",
                     dbg_msg_total_, dbg_element_msg_total_, dbg_facedetect_msg_total_,
                     dbg_face0_msg_total_, dbg_face_prev_msg_total_,
                     sn ? sn : "(null)", src_name ? src_name : "(null)");
            }
        } else if (now_dbg - dbg_last_log_ >= std::chrono::seconds(2)) {
            dbg_last_log_ = now_dbg;
            LOGI("face_branch[HUNT]: bus msg total={} element={} facedetect={} face0={} prev={} (last non-element type={})",
                 dbg_msg_total_, dbg_element_msg_total_, dbg_facedetect_msg_total_,
                 dbg_face0_msg_total_, dbg_face_prev_msg_total_,
                 static_cast<int>(t));
        }
    }

    /* 仅消费 facedetect 主检测的 element message 与 ERROR；其他消息一律放行。 */
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GstObject* src = GST_MESSAGE_SRC(msg);
        if (src && std::string(GST_OBJECT_NAME(src)) == "face0") {
            std::lock_guard<std::mutex> lk(mu_);
            GError*  e = nullptr;
            gchar*   d = nullptr;
            gst_message_parse_error(msg, &e, &d);
            LOGE("face_branch: face0 reported error: {} ({})",
                 e ? e->message : "(null)", d ? d : "(null)");
            if (e) g_error_free(e);
            if (d) g_free(d);
            /* 触发硬保护：关掉 face_valve，避免错误反复刷日志。 */
            if (GstElement* fv = element("face_valve")) {
                g_object_set(fv, "drop", TRUE, nullptr);
            }
        }
        return;
    }

    if (GST_MESSAGE_TYPE(msg) != GST_MESSAGE_ELEMENT) return;

    const GstStructure* s = gst_message_get_structure(msg);
    if (!s || !gst_structure_has_name(s, "facedetect")) return;

    GstObject* src = GST_MESSAGE_SRC(msg);
    /* 主检测路径名 = face0；preview 副线那个 facedetect 没有 name，gst 会自动给一个
     * "facedetect0" 之类的内部名，靠这一行过滤掉，避免 preview 副线的检测重复进 last_state_。 */
    if (!src || std::string(GST_OBJECT_NAME(src)) != "face0") return;

    FaceEvent ev;
    if (!parse_faces_message(s, ev)) return;

    /* emit_when_empty=false 时，count=0 直接丢弃，避免空检测刷屏。 */
    if (!cfg_.control.emit_when_empty && ev.count == 0) return;

    auto now = std::chrono::steady_clock::now();

    std::unique_lock<std::mutex> lk(mu_);

    /* cooldown_ms 节流：窗口内仅保留最新一条。 */
    if (last_emit_ts_.time_since_epoch().count() != 0 &&
        now - last_emit_ts_ < std::chrono::milliseconds(cfg_.control.cooldown_ms)) {
        last_state_   = std::move(ev);          // 仍更新缓存，但不日志
        last_state_.ts = now;
        return;
    }
    last_emit_ts_  = now;
    last_state_    = std::move(ev);
    last_state_.ts = now;

    /* rate-limited 1Hz LOGI（避免高密度场景下日志爆量）。 */
    if (now - last_log_ts_ >= std::chrono::seconds(1)) {
        last_log_ts_ = now;
        if (!last_state_.rects.empty()) {
            const auto& r = last_state_.rects.front();
            LOGI("face_branch: detected count={} top_rect={}x{}@({},{})",
                 last_state_.count, r.w, r.h, r.x, r.y);
        } else {
            LOGI("face_branch: detected count=0");
        }
    }

    /* 回调推送（events FIFO 等）：拷贝所需数据到栈上，释放 mu_ 后再同步调用，
     * 避免回调内反手抓锁造成死锁。此时 last_state_ 已更新，拷贝过去即可。 */
    OnFacesCallback cb_local = on_faces_cb_;
    FaceEvent       ev_copy  = last_state_;
    int             fw       = frame_w_;
    int             fh       = frame_h_;
    lk.unlock();

    if (cb_local) {
        cb_local(ev_copy, fw, fh);
    }
}

bool FaceBranch::parse_faces_message(const GstStructure* s, FaceEvent& out) {
    if (!s) return false;

    /* faces 字段在不同 GStreamer 版本里类型可能是 GValueArray、GST_TYPE_LIST
     * 或 GST_TYPE_ARRAY，三者都用 gst_value_list_get_size 兜底访问。 */
    const GValue* v = gst_structure_get_value(s, "faces");
    if (!v) {
        out.count = 0;
        return true;
    }

    const auto try_list_size = [](const GValue* x) -> int {
        if (G_VALUE_HOLDS(x, GST_TYPE_LIST))  return static_cast<int>(gst_value_list_get_size(x));
        if (G_VALUE_HOLDS(x, GST_TYPE_ARRAY)) return static_cast<int>(gst_value_array_get_size(x));
        return -1;
    };
    const auto try_list_at = [](const GValue* x, int i) -> const GValue* {
        if (G_VALUE_HOLDS(x, GST_TYPE_LIST))  return gst_value_list_get_value(x, i);
        if (G_VALUE_HOLDS(x, GST_TYPE_ARRAY)) return gst_value_array_get_value(x, i);
        return nullptr;
    };

    /* facedetect 元素实际把 x/y/width/height 存为 G_TYPE_UINT（见 gst-plugins-bad
     * opencv/gstfacedetect.c）。gst_structure_get_int 遇 uint 字段会返回 FALSE 且
     * 不写 out 参数——旧代码只调 get_int，导致 count 正常但 rect 全 0。
     * 这里统一用 "uint 优先、int 兜底" 的取值 helper，兼容不同 GStreamer 版本。 */
    const auto get_field_int = [](const GstStructure* fs, const char* key, int* out_v) {
        guint u = 0;
        if (gst_structure_get_uint(fs, key, &u)) { *out_v = static_cast<int>(u); return; }
        gint  s = 0;
        if (gst_structure_get_int (fs, key, &s)) { *out_v = s;                    return; }
        /* 保底：GValue 泛读，处理跨版本可能出现的 int64/uint64。 */
        const GValue* gv = gst_structure_get_value(fs, key);
        if (!gv) return;
        if (G_VALUE_HOLDS_UINT  (gv)) { *out_v = static_cast<int>(g_value_get_uint  (gv)); return; }
        if (G_VALUE_HOLDS_INT   (gv)) { *out_v = g_value_get_int   (gv); return; }
        if (G_VALUE_HOLDS_UINT64(gv)) { *out_v = static_cast<int>(g_value_get_uint64(gv)); return; }
        if (G_VALUE_HOLDS_INT64 (gv)) { *out_v = static_cast<int>(g_value_get_int64 (gv)); return; }
    };

    int n = try_list_size(v);
    if (n < 0) {
        /* 兜底：GValueArray（旧 API）。 */
        if (G_VALUE_HOLDS_BOXED(v)) {
            GValueArray* arr = static_cast<GValueArray*>(g_value_get_boxed(v));
            if (!arr) { out.count = 0; return true; }
            n = static_cast<int>(arr->n_values);
            std::vector<FaceRect> rects;
            rects.reserve(n);
            for (int i = 0; i < n; ++i) {
                GValue* face_v = g_value_array_get_nth(arr, i);
                if (!face_v || !GST_VALUE_HOLDS_STRUCTURE(face_v)) continue;
                const GstStructure* fs = gst_value_get_structure(face_v);
                FaceRect r;
                get_field_int(fs, "x",      &r.x);
                get_field_int(fs, "y",      &r.y);
                get_field_int(fs, "width",  &r.w);
                get_field_int(fs, "height", &r.h);
                rects.push_back(r);
            }
            std::sort(rects.begin(), rects.end(),
                      [](const FaceRect& a, const FaceRect& b){ return a.area() > b.area(); });
            out.count = static_cast<int>(rects.size());
            if (static_cast<int>(rects.size()) > kMaxFaces) {
                rects.resize(kMaxFaces);
            }
            out.rects = std::move(rects);
            return true;
        }
        out.count = 0;
        return true;
    }

    std::vector<FaceRect> rects;
    rects.reserve(n);
    for (int i = 0; i < n; ++i) {
        const GValue* face_v = try_list_at(v, i);
        if (!face_v || !GST_VALUE_HOLDS_STRUCTURE(face_v)) continue;
        const GstStructure* fs = gst_value_get_structure(face_v);
        FaceRect r;
        get_field_int(fs, "x",      &r.x);
        get_field_int(fs, "y",      &r.y);
        get_field_int(fs, "width",  &r.w);
        get_field_int(fs, "height", &r.h);
        rects.push_back(r);
    }
    std::sort(rects.begin(), rects.end(),
              [](const FaceRect& a, const FaceRect& b){ return a.area() > b.area(); });
    out.count = static_cast<int>(rects.size());
    if (static_cast<int>(rects.size()) > kMaxFaces) {
        rects.resize(kMaxFaces);
    }
    out.rects = std::move(rects);
    return true;
}
