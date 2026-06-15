//
// Created by vompom on 2026/06/15.
//
// @Description
//   见 record.h 顶部说明。
//

#include "record.h"
#include "log.h"

#include <ctime>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

void Record::configure(const RecordConfig& cfg) {
    cfg_ = cfg;
    if (cfg_.segment_sec <= 0) cfg_.segment_sec = 60;
    if (cfg_.filename_pattern.empty()) cfg_.filename_pattern = "%Y%m%d_%H%M%S.mp4";

    if (!cfg_.dir.empty()) {
        std::error_code ec;
        fs::create_directories(cfg_.dir, ec);
        if (ec) {
            LOGW("record: create_directories({}) failed: {}", cfg_.dir, ec.message());
        }
    }

    LOGI("record: configured dir='{}' segment={}s pattern='{}' enabled_at_start={}",
         cfg_.dir, cfg_.segment_sec, cfg_.filename_pattern, cfg_.enabled);
}

bool Record::on_attached_locked() {
    /* 调用方（BranchBase::attach_to_media）已持 mu_。 */
    GstElement* sink = element("rec_sink");
    if (!sink) return false;   // 理论上不会发生：required_elements 已校验过

    /* splitmuxsink 要求 location 含一个 sprintf 整数占位才能通过校验；实际
     * 文件名由 "format-location" 信号回调返回的字符串覆盖，这里占位不会被用。 */
    g_object_set(sink, "location", "/tmp/vm_iot_rec_unused_%05d.mp4", nullptr);

    /* format-location 回调返值由 splitmuxsink g_free，回调内必须 g_strdup 分配。 */
    if (format_loc_handler_ == 0) {
        format_loc_handler_ = g_signal_connect(
            sink, "format-location",
            G_CALLBACK(&Record::on_format_location), this);
    }

    /* 在 pipeline bus 上订阅 "splitmuxsink-fragment-closed" element message，用作
     * "段 mp4 已落盘写完 moov" 的同步信号。
     *
     * 纪律：GstRTSPMedia 已在 pipeline bus 上挂了自己的 sync_handler，
     * 所以**不能**用 gst_bus_set_sync_handler（会覆盖导致 pipeline 卡住）。
     * 改用 enable_sync_message_emission + sync-message::element 信号，多订阅者
     * 互不干扰。持 bus_ref_ 一份 ref，detach 时用以摸信号 + 关 emission。 */
    if (!sync_handler_set_) {
        if (GstElement* pipe = pipeline_locked()) {
            GstBus* bus = gst_element_get_bus(pipe);
            if (bus) {
                gst_bus_enable_sync_message_emission(bus);
                sync_msg_handler_ = g_signal_connect(
                    bus, "sync-message::element",
                    G_CALLBACK(&Record::on_sync_message), this);
                bus_ref_ = bus;   // 转移 ref（不 unref）
                sync_handler_set_ = true;
            } else {
                LOGW("record: pipeline has no bus; fragment-closed wait will time out");
            }
        }
    }

    /* 按 cfg.enabled 决定是否开闸。 */
    set_valve_drop_locked(!cfg_.enabled);
    recording_ = cfg_.enabled;
    LOGI("record: ready (recording={}, dir='{}', pattern='{}')",
         recording_ ? "on" : "off", cfg_.dir, cfg_.filename_pattern);
    return true;
}

void Record::on_detaching_locked() {
    /* 元素马上要被基类 unref，先把可能异步访问元素的 timeout 停掉。 */
    cancel_auto_timeout_locked_();

    /* 让 splitmuxsink finalize"当前段"（mp4mux + async-finalize=true 下，当前
     * 未切片的段不写 moov，不 finalize 则不可播）。
     *   recording_=false：上次 stop 已 finalize，直接清理。
     *   recording_=true ：用户未主动 stop（如 SIGINT）—— valve 关闸 →
     *                       给 rec_queue.src 注入 EOS 推到 splitmuxsink 触发 finalize
     *                       → 等 fragment-closed（≤1.5s）。 */
    if (recording_) {
        LOGI("record: shutdown while recording, finalizing current segment via EOS");
        set_valve_drop_locked(true);
        fragment_closed_pending_ = false;
        inject_eos_locked_();
        if (!wait_fragment_closed_locked_(1500)) {
            LOGW("record: fragment-closed not seen within timeout; last segment may be incomplete");
        }
        recording_ = false;
    }

    /* 摸 sync-message 信号 + 关 emission：元素 unref 前必须断开，避免后续消息
     * 派发到已悬空的 this。用 attach 时持的 bus_ref_（pipeline NULL 态下重取
     * 可能拿到新 bus）。 */
    if (sync_handler_set_) {
        if (bus_ref_) {
            if (sync_msg_handler_) {
                g_signal_handler_disconnect(bus_ref_, sync_msg_handler_);
                sync_msg_handler_ = 0;
            }
            gst_bus_disable_sync_message_emission(bus_ref_);
            gst_object_unref(bus_ref_);
            bus_ref_ = nullptr;
        }
        sync_handler_set_ = false;
    }

    /* 断开 format-location 信号。 */
    if (format_loc_handler_) {
        if (GstElement* sink = element("rec_sink")) {
            g_signal_handler_disconnect(sink, format_loc_handler_);
        }
        format_loc_handler_ = 0;
    }
}

bool Record::is_recording() const {
    std::lock_guard<std::mutex> lk(mu_);
    return recording_;
}

void Record::set_valve_drop_locked(bool drop) {
    GstElement* valve = element("rec_valve");
    if (valve) g_object_set(valve, "drop", drop ? TRUE : FALSE, nullptr);
}

bool Record::ensure_ready_locked_(std::string& err) const {
    if (!element("rec_valve") || !element("rec_sink")) {
        err = "pipeline_not_ready";
        return false;
    }
    return true;
}

void Record::cancel_auto_timeout_locked_() {
    if (auto_timeout_id_) {
        g_source_remove(auto_timeout_id_);
        auto_timeout_id_ = 0;
    }
}

bool Record::start(std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensure_ready_locked_(err)) return false;
    /* 手动 start 切回常开模式：取消可能存在的 auto 定时器。 */
    cancel_auto_timeout_locked_();
    if (recording_) return true;          // 幂等
    set_valve_drop_locked(false);
    recording_ = true;
    LOGI("record: start (manual)");
    return true;
}

bool Record::stop_recording(std::string& err) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!ensure_ready_locked_(err)) return false;
    cancel_auto_timeout_locked_();
    if (!recording_) return true;         // 幂等

    /* ① emit "split-now"让 splitmuxsink 在下一关键帧切片 + async finalize 当前段
     *    （send-keyframe-requests=true 会主动请求 keyframe，最多延迟 1 个 GOP）。
     * ② 等 fragment-closed 确认落盘（≤1s）。
     * ③ valve 关闸，避免 splitmuxsink 自动开下一段产生空文件。 */
    fragment_closed_pending_ = false;
    LOGI("record: stop -> split-now, waiting for fragment-closed");
    g_signal_emit_by_name(element("rec_sink"), "split-now", nullptr);
    bool ok = wait_fragment_closed_locked_(1000);
    set_valve_drop_locked(true);
    recording_ = false;
    if (!ok) {
        LOGW("record: fragment-closed not seen within timeout; segment may still be writing");
    } else {
        LOGI("record: stop done (segment finalized)");
    }
    return true;
}

gboolean Record::on_auto_timeout(gpointer user) {
    auto* self = static_cast<Record*>(user);
    if (!self) return G_SOURCE_REMOVE;
    /* 走标准 stop 路径以确保 finalize 一致。auto_timeout_id_ 由 stop 路径前清零。 */
    self->auto_timeout_id_ = 0;       // 标记已触发（在 GMainLoop 线程，无并发）
    std::string err;
    if (!self->stop_recording(err)) {
        LOGW("record: auto stop failed: {}", err);
    } else {
        LOGI("record: stop (auto timeout)");
    }
    return G_SOURCE_REMOVE;
}

bool Record::auto_record(int duration_sec, std::string& err) {
    if (duration_sec <= 0) {
        err = "invalid_duration";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensure_ready_locked_(err)) return false;

    /* 先撤掉旧 timeout，再装新的。 */
    cancel_auto_timeout_locked_();
    set_valve_drop_locked(false);
    recording_ = true;
    auto_timeout_id_ = g_timeout_add_seconds(static_cast<guint>(duration_sec),
                                             &Record::on_auto_timeout, this);
    LOGI("record: auto start ({}s)", duration_sec);
    return true;
}

void Record::format_status(std::string& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    bool attached = element("rec_valve") != nullptr && element("rec_sink") != nullptr;
    out += "record_attached=";
    out += attached ? "true\n" : "false\n";
    out += "record_recording=";
    out += recording_ ? "true\n" : "false\n";
    out += "record_dir=" + cfg_.dir + "\n";
    out += "record_segment_sec=" + std::to_string(cfg_.segment_sec) + "\n";
}

gchar* Record::on_format_location(GstElement* /*sink*/, guint fragment_id, gpointer user) {
    auto* self = static_cast<Record*>(user);
    if (!self) return g_strdup("/tmp/vm_iot_rec_fallback.mp4");

    /* 流线程回调：cfg_ 启动期一次性写入后只读，无需持锁。 */
    const RecordConfig& cfg = self->cfg_;

    /* 1) 用 strftime 处理日期模板。模板里若不含 % 也无害——strftime 直接原样返回。
     *    缓冲区 256 足够容纳常规 "%Y%m%d_%H%M%S.mp4" 风格扩展后的串。 */
    char date_buf[256] = {0};
    std::time_t now = std::time(nullptr);
    std::tm tm_local{};
    localtime_r(&now, &tm_local);
    if (std::strftime(date_buf, sizeof(date_buf),
                      cfg.filename_pattern.c_str(), &tm_local) == 0) {
        /* strftime 失败（缓冲不够 / 模板非法）→ 退化到 epoch 秒。 */
        std::snprintf(date_buf, sizeof(date_buf),
                      "rec_%lld.mp4", static_cast<long long>(now));
    }

    /* 2) 在扩展名之前插入 _%05u 段号；没有扩展名则末尾追加。 */
    std::string name = date_buf;
    char seq[16];
    std::snprintf(seq, sizeof(seq), "_%05u", fragment_id);
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        name += seq;
        name += ".mp4";
    } else {
        name.insert(dot, seq);
    }

    /* 3) 拼上输出目录。 */
    std::string full;
    if (cfg.dir.empty()) {
        full = std::move(name);
    } else if (cfg.dir.back() == '/') {
        full = cfg.dir + name;
    } else {
        full = cfg.dir + "/" + name;
    }

    LOGI("record: new segment #{} -> {}", fragment_id, full);
    return g_strdup(full.c_str());
}

void Record::on_sync_message(GstBus* /*bus*/, GstMessage* msg, gpointer user) {
    auto* self = static_cast<Record*>(user);
    if (!self || !msg) return;

    /* sync-message::element 过滤后只剩 ELEMENT 类型，但稳妥起见再判一次。
     * 仅识别 splitmuxsink 发出的 "splitmuxsink-fragment-closed"。
     *
     * 严格纪律：本回调跑在元素流线程，**绝不能持业务锁阻塞**。
     * 直接写 bool（C++ 标准下 bool 写非原子，但单写者+单消费者，且后续
     * notify_all 提供 release 语义足以建立 happens-before）+ notify_all 即可。
     * 不持 mu_：避免与 stop/detach 持锁等 cv 时形成回环。 */
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT) {
        const GstStructure* s = gst_message_get_structure(msg);
        if (s && gst_structure_has_name(s, "splitmuxsink-fragment-closed")) {
            self->fragment_closed_pending_ = true;
            self->fragment_cv_.notify_all();
        }
    }
}

bool Record::wait_fragment_closed_locked_(int timeout_ms) {
    if (timeout_ms <= 0) return false;
    /* 调用方持 mu_，转换为 unique_lock 给 cv 用。 */
    std::unique_lock<std::mutex> lk(mu_, std::adopt_lock);
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    bool ok = fragment_cv_.wait_until(lk, deadline,
                                      [this]{ return fragment_closed_pending_; });
    fragment_closed_pending_ = false;
    lk.release();   // 还回给调用方继续持有
    return ok;
}

void Record::inject_eos_locked_() {
    GstElement* q = element("rec_queue");
    if (!q) {
        LOGW("record: rec_queue not found, cannot inject EOS");
        return;
    }
    /* 从 rec_queue.src 推 EOS：沿 valve → splitmuxsink 向下游传递，
     * 让 splitmuxsink 把当前段 finalize。
     * 注意 EOS 只沿这一支下行，不会回灌到 enc_t（tee 分支独立）。 */
    GstPad* src = gst_element_get_static_pad(q, "src");
    if (!src) {
        LOGW("record: rec_queue.src pad missing, cannot inject EOS");
        return;
    }
    gboolean sent = gst_pad_send_event(src, gst_event_new_eos());
    gst_object_unref(src);
    if (!sent) {
        LOGW("record: gst_pad_send_event(EOS) returned FALSE");
    }
}