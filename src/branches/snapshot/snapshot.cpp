//
// Created by vompom on 2026/06/11.
//
// @Description
//   见 snapshot.h 顶部说明。
//

#include "snapshot.h"
#include "log.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

void Snapshot::configure(const std::string& dir, int /*quality_unused*/, int timeout_ms) {
    dir_        = dir;
    timeout_ms_ = (timeout_ms > 0 ? timeout_ms : 1500);

    if (!dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        if (ec) {
            LOGW("snapshot: create_directories({}) failed: {}", dir_, ec.message());
        }
    }
    LOGI("snapshot: configured dir='{}' timeout={}ms", dir_, timeout_ms_);
}

void Snapshot::attach_to_media(GstRTSPMedia* media) {
    GstElement* pipeline = gst_rtsp_media_get_element(media);
    if (!pipeline) {
        LOGW("snapshot: gst_rtsp_media_get_element returned null");
        return;
    }
    GstElement* valve = gst_bin_get_by_name(GST_BIN(pipeline), "snap_valve");
    GstElement* sink  = gst_bin_get_by_name(GST_BIN(pipeline), "snap_sink");
    if (!valve || !sink) {
        LOGW("snapshot: snap_valve/snap_sink not found (valve={}, sink={})",
             (void*)valve, (void*)sink);
        if (valve) gst_object_unref(valve);
        if (sink)  gst_object_unref(sink);
        gst_object_unref(pipeline);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        // 替换旧引用（只服务最新一次 media-configure）。
        if (pipeline_) gst_object_unref(pipeline_);
        if (valve_)    gst_object_unref(valve_);
        if (sink_)     gst_object_unref(sink_);
        pipeline_ = pipeline;   // 保留 ref（gst_rtsp_media_get_element 已加 ref）
        valve_    = valve;      // 保留 ref（gst_bin_get_by_name 已加 ref）
        sink_     = sink;
    }

    g_signal_connect(media, "unprepared",
                     G_CALLBACK(&Snapshot::on_media_unprepared), this);

    LOGI("snapshot: attached to pipeline (valve+sink ready)");
}

void Snapshot::on_media_unprepared(GstRTSPMedia* /*media*/, gpointer user) {
    auto* self = static_cast<Snapshot*>(user);
    if (!self) return;
    std::lock_guard<std::mutex> lk(self->mu_);
    if (self->pipeline_) { gst_object_unref(self->pipeline_); self->pipeline_ = nullptr; }
    if (self->valve_)    { gst_object_unref(self->valve_);    self->valve_    = nullptr; }
    if (self->sink_)     { gst_object_unref(self->sink_);     self->sink_     = nullptr; }
    LOGI("snapshot: media unprepared, branch detached");
}

void Snapshot::shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
    if (valve_)    { gst_object_unref(valve_);    valve_    = nullptr; }
    if (sink_)     { gst_object_unref(sink_);     sink_     = nullptr; }
}

bool Snapshot::ready() const {
    std::lock_guard<std::mutex> lk(mu_);
    return valve_ != nullptr && sink_ != nullptr && pipeline_ != nullptr;
}

std::string Snapshot::make_default_path() const {
    // 时间戳精度到毫秒，避免同秒多张冲突。
    auto now    = std::chrono::system_clock::now();
    auto sec    = std::chrono::system_clock::to_time_t(now);
    auto ms     = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    localtime_r(&sec, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "snap_%04d%02d%02d_%02d%02d%02d_%03lld.jpg",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  (long long)ms);
    std::string base = dir_.empty() ? std::string("/tmp") : dir_;
    return base + "/" + buf;
}

void Snapshot::set_valve_drop(bool drop) {
    // 调用方应已持锁；这里只做属性写入。
    if (valve_) g_object_set(valve_, "drop", drop ? TRUE : FALSE, nullptr);
}

GstPadProbeReturn Snapshot::on_sink_buffer(GstPad* /*pad*/, GstPadProbeInfo* /*info*/, gpointer user) {
    auto* self = static_cast<Snapshot*>(user);
    if (!self) return GST_PAD_PROBE_REMOVE;
    {
        std::lock_guard<std::mutex> lk(self->cv_mu_);
        self->got_buffer_ = true;
    }
    self->cv_.notify_all();
    /* 一次性 probe：multifilesink 是同步写，buffer 到 sink pad 时
     * write(2) 已经返回，文件已经在盘上。立即移除自己，避免后续帧再触发。 */
    return GST_PAD_PROBE_REMOVE;
}

bool Snapshot::take(std::string& out_path, std::string& err) const
{
    /* take() 串行化：避免两次并发截图争抢同一个 valve / probe。 */
    std::lock_guard<std::mutex> serial(take_mu_);

    // 1) 校验状态
    GstElement* pipeline = nullptr;
    GstElement* valve    = nullptr;
    GstElement* sink     = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!pipeline_ || !valve_ || !sink_) {
            err = "pipeline_not_ready";
            return false;
        }
        // 在锁内 ref 一份，锁外使用，避免 take 期间 unprepared 把元素拆掉。
        pipeline = GST_ELEMENT(gst_object_ref(pipeline_));
        valve    = GST_ELEMENT(gst_object_ref(valve_));
        sink     = GST_ELEMENT(gst_object_ref(sink_));
    }

    // 2) 决定输出路径
    if (out_path.empty()) {
        out_path = make_default_path();
    }
    // 确保目录存在
    {
        std::filesystem::path p(out_path);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
            if (ec) {
                err = "invalid_path";
                gst_object_unref(pipeline);
                gst_object_unref(valve);
                gst_object_unref(sink);
                return false;
            }
        }
    }

    // 3) 在 sink pad 上挂一次性 BUFFER probe
    //    放在开闸之前，确保不会漏掉第一帧。
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        got_buffer_ = false;
    }
    GstPad* sink_pad = gst_element_get_static_pad(sink, "sink");
    gulong  probe_id = 0;
    if (sink_pad) {
        probe_id = gst_pad_add_probe(
            sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
            &Snapshot::on_sink_buffer,
            const_cast<Snapshot*>(this), nullptr);
    } else {
        err = "pipeline_not_ready";
        gst_object_unref(pipeline);
        gst_object_unref(valve);
        gst_object_unref(sink);
        return false;
    }

    // 4) 设置 sink location & 开闸
    g_object_set(sink, "location", out_path.c_str(), nullptr);
    g_object_set(valve, "drop", FALSE, nullptr);

    // 5) 等待 probe 通知（带超时）
    bool ok = false;
    {
        std::unique_lock<std::mutex> lk(cv_mu_);
        ok = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms_),
                          [this]{ return got_buffer_; });
    }

    // 6) 关闸（无论成功失败）
    g_object_set(valve, "drop", TRUE, nullptr);

    // 7) 移除 probe（probe 自身返回 REMOVE 时也会自动移除，
    //    但超时分支需要主动清理；重复 remove 是安全的）
    if (probe_id != 0 && sink_pad) {
        gst_pad_remove_probe(sink_pad, probe_id);
    }
    if (sink_pad) gst_object_unref(sink_pad);

    gst_object_unref(pipeline);
    gst_object_unref(valve);
    gst_object_unref(sink);

    if (!ok) {
        err = "timeout";
        // 清理可能写到一半的文件
        struct stat st{};
        if (::stat(out_path.c_str(), &st) == 0 && st.st_size == 0) {
            ::remove(out_path.c_str());
        }
        return false;
    }

    LOGI("snapshot: written {}", out_path);
    return true;
}
