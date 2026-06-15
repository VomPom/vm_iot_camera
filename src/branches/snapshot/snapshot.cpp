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

    // 1) 校验状态 + 在锁内 ref 一份元素，锁外使用，避免 take 期间 unprepared 把元素拆掉。
    GstElement* valve = nullptr;
    GstElement* sink  = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        GstElement* v = element("snap_valve");
        GstElement* s = element("snap_sink");
        if (!v || !s) {
            err = "pipeline_not_ready";
            return false;
        }
        valve = GST_ELEMENT(gst_object_ref(v));
        sink  = GST_ELEMENT(gst_object_ref(s));
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
