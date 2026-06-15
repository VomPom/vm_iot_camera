//
// Created by vompom on 2026/06/15.
//
// @Description
//   见 branch_base.h 顶部说明。
//

#include "branch_base.h"
#include "log.h"

BranchBase::~BranchBase() {
    /* 析构兜底：正常路径下 main 会先调 shutdown()，但保留这层防止子类忘记。 */
    std::lock_guard<std::mutex> lk(mu_);
    detach_locked_();
}

void BranchBase::attach_to_media(GstRTSPMedia* media) {
    const char* tag = branch_name();

    GstElement* pipeline = gst_rtsp_media_get_element(media);
    if (!pipeline) {
        LOGW("{}: gst_rtsp_media_get_element returned null", tag);
        return;
    }

    /* 1) 把所有 required 元素抓出来（任一失败则全部回滚）。 */
    auto names = required_elements();
    std::unordered_map<std::string, GstElement*> got;
    got.reserve(names.size());
    bool all_ok = true;

    for (const char* n : names) {
        if (!n || !*n) { all_ok = false; break; }
        GstElement* e = gst_bin_get_by_name(GST_BIN(pipeline), n);
        if (!e) {
            LOGW("{}: required element '{}' not found in pipeline", tag, n);
            all_ok = false;
            break;
        }
        got.emplace(std::string(n), e);
    }

    if (!all_ok) {
        for (auto& kv : got) gst_object_unref(kv.second);
        gst_object_unref(pipeline);
        return;
    }

    /* 2) 锁内安装新引用（如有旧引用先走一次 detach）。 */
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pipeline_ || !elements_.empty()) {
            LOGI("{}: re-attach detected, releasing previous refs first", tag);
            detach_locked_();
        }
        pipeline_  = pipeline;          // 转移 ref（gst_rtsp_media_get_element 已加 ref）
        elements_  = std::move(got);    // 转移每个 by_name 加的 ref

        /* 3) 持锁回调子类做一次性初始化。子类返回 false 视为 attach 失败回滚。 */
        if (!on_attached_locked()) {
            LOGW("{}: on_attached_locked rejected, rolling back attach", tag);
            detach_locked_();
            return;
        }
    }

    /* 4) 挂 unprepared 信号（在锁外，避免回调反向加锁的潜在死锁）。 */
    g_signal_connect(media, "unprepared",
                     G_CALLBACK(&BranchBase::s_on_unprepared), this);

    LOGI("{}: attached to pipeline ({} element(s))", tag, (int)elements_.size());
}

void BranchBase::shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    detach_locked_();
}

bool BranchBase::ready() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pipeline_ != nullptr;
}

GstElement* BranchBase::element(const char* name) const {
    if (!name) return nullptr;
    auto it = elements_.find(name);
    return it == elements_.end() ? nullptr : it->second;
}

void BranchBase::s_on_unprepared(GstRTSPMedia* /*media*/, gpointer user) {
    auto* self = static_cast<BranchBase*>(user);
    if (!self) return;
    std::lock_guard<std::mutex> lk(self->mu_);
    self->detach_locked_();
    LOGI("{}: media unprepared, branch detached", self->branch_name());
}

void BranchBase::detach_locked_() {
    /* 顺序很关键：先让子类停掉所有"还在用元素"的异步活动，
     * 再 unref 元素本身。子类可能在 on_detaching_locked 里访问 element()。 */
    if (pipeline_ || !elements_.empty()) {
        on_detaching_locked();
    }
    for (auto& kv : elements_) {
        if (kv.second) gst_object_unref(kv.second);
    }
    elements_.clear();
    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}
