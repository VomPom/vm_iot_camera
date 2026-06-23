//
// Created by vompom on 2026/06/22.
//
// @Description
//   见 pag_overlay.h 顶部说明。
//

#include "pag_overlay.h"

#include <gst/gst.h>

#include "log.h"

void PagOverlay::configure(const std::string& abs_pag_file_path) {
    /* 启动期单次配置；attach 之前调用，无需加锁。 */
    pag_file_path_ = abs_pag_file_path;
    LOGI("pag_overlay: configured pag-file='{}'",
         pag_file_path_.empty() ? "(empty, passthrough)" : pag_file_path_.c_str());
}

bool PagOverlay::on_attached_locked() {
    /* BranchBase 已经把 "pag0" 抓到了 elements_，且当前在 mu_ 保护下、
     * pagfilter 还处于 NULL/READY（PAUSED 之前），是注入 pag-file 的唯一窗口。
     *
     * 空路径走 passthrough：让 pagfilter 自己在 set_caps 时按 should_render()=false
     * 启用 GST_BASE_TRANSFORM_FLAG_PASSTHROUGH，零开销。 */
    GstElement* pag0 = element("pag0");
    if (!pag0) {
        /* 理论不会发生（required_elements 已确保），但保留兜底日志便于排错。 */
        LOGW("pag_overlay: element 'pag0' missing in on_attached_locked");
        return false;
    }

    if (pag_file_path_.empty()) {
        /* 显式写空也走一遍，保证幂等：pagfilter 内部把空串视为 nullptr。 */
        g_object_set(pag0, "pag-file", "", nullptr);
        LOGI("pag_overlay: pag-file left empty, filter stays in passthrough");
        return true;
    }

    g_object_set(pag0, "pag-file", pag_file_path_.c_str(), nullptr);
    LOGI("pag_overlay: injected pag-file='{}' into pag0", pag_file_path_);
    return true;
}

/* ───────────────────── 运行时热控实现 ─────────────────── */

bool PagOverlay::set_pag_file(const std::string& abs_path, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* pag0 = element("pag0");
    if (!pag0) {
        err = "not_attached";
        return false;
    }
    /* pagfilter 已支持 GST_PARAM_MUTABLE_PLAYING；PLAYING 状态下变更会被
     * 内部排队到 streaming 线程下一帧消费。 */
    g_object_set(pag0, "pag-file", abs_path.c_str(), nullptr);
    pag_file_path_ = abs_path;
    LOGI("pag_overlay: hot-set pag-file='{}'", abs_path);
    return true;
}

bool PagOverlay::set_text(int idx, const std::string& utf8, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* pag0 = element("pag0");
    if (!pag0) {
        err = "not_attached";
        return false;
    }
    if (idx < 0) {
        err = "invalid_idx";
        return false;
    }
    /* pag-text 协议："<idx>:<utf8>"，utf8 中允许出现冒号/空格（pagfilter 内部
     * 用 strchr 找首个冒号定位 idx，剩余 raw 全部当 utf8）。 */
    std::string combined = std::to_string(idx) + ":" + utf8;
    g_object_set(pag0, "pag-text", combined.c_str(), nullptr);
    LOGI("pag_overlay: hot-set pag-text idx={} len={}", idx, utf8.size());
    return true;
}

bool PagOverlay::set_replace_image_idx(int idx, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* pag0 = element("pag0");
    if (!pag0) {
        err = "not_attached";
        return false;
    }
    g_object_set(pag0, "pag-replace-image-idx", idx, nullptr);
    LOGI("pag_overlay: hot-set pag-replace-image-idx={}", idx);
    return true;
}

bool PagOverlay::set_replace_image_every(int every, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* pag0 = element("pag0");
    if (!pag0) {
        err = "not_attached";
        return false;
    }
    if (every < 1) {
        err = "invalid_every";
        return false;
    }
    g_object_set(pag0, "pag-replace-image-every", every, nullptr);
    LOGI("pag_overlay: hot-set pag-replace-image-every={}", every);
    return true;
}

PagOverlay::StatusSnapshot PagOverlay::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    StatusSnapshot s;
    /* element() 不增 ref，但我们在持锁期间访问，BranchBase 保证元素引用
     * 在 detach_locked_ 之前一直有效。 */
    GstElement* pag0 = const_cast<PagOverlay*>(this)->element("pag0");
    if (!pag0) {
        s.attached = false;
        return s;
    }
    s.attached = true;
    gchar* path = nullptr;
    g_object_get(pag0,
                 "pag-file",                  &path,
                 "pag-replace-image-idx",     &s.replace_idx,
                 "pag-replace-image-every",   &s.replace_every,
                 nullptr);
    if (path) {
        s.pag_file = path;
        g_free(path);
    }
    return s;
}

void PagOverlay::format_status(std::string& out) const {
    auto s = snapshot();
    out.append("pag_attached=").append(s.attached ? "true" : "false").append("\n");
    if (s.attached) {
        out.append("pag_file=").append(s.pag_file).append("\n");
        out.append("pag_replace_idx=").append(std::to_string(s.replace_idx)).append("\n");
        out.append("pag_replace_every=").append(std::to_string(s.replace_every)).append("\n");
    }
}