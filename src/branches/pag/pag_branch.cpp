//
// Created by vompom on 2026/06/22.
//
// @Description
//   见 pag_branch.h 顶部说明。本文件仅实现 PagBranch 基类。
//   子类 PagSticker / PagEffect 见 pag_sticker.cpp / pag_effect.cpp。
//

#include "pag_branch.h"

#include <gst/gst.h>

#include <cstdio>

#include "log.h"

void PagBranch::configure(const PagFilterConfig& cfg, const std::string& abs_pag_file_path) {
    /* 启动期单次配置；attach 之前调用，无需加锁。
     * 子类如需追加自身字段（目前没有），可 override 后先调 PagBranch::configure。 */
    pag_file_path_ = abs_pag_file_path;
    type_  = cfg.type;
    pos_x_ = cfg.pos_x;
    pos_y_ = cfg.pos_y;
    scale_ = cfg.scale;
    LOGI("pag_branch: configured type='{}' pag-file='{}' pos=({:.3f},{:.3f}) scale={:.3f}",
         to_string(type_),
         pag_file_path_.empty() ? "(empty, passthrough)" : pag_file_path_.c_str(),
         pos_x_, pos_y_, scale_);
}

bool PagBranch::on_attached_locked() {
    /* BranchBase 已经把 "pag0" 抓到了 elements_，且当前在 mu_ 保护下、
     * pagfilter 还处于 NULL/READY（PAUSED 之前），是注入初始属性的唯一窗口。
     *
     * 注入顺序：先 pag-type（MUTABLE_READY，必须最早设），再调 inject_type_specific_locked
     * 让子类注入位置/缩放或替换轨道相关属性，最后 pag-file。这样保证子类属性
     * 都在 file 之前就位，pagfilter 一旦在后续 set_caps 加载 .pag 时就能用。 */
    GstElement* pag0 = element("pag0");
    if (!pag0) {
        /* 理论不会发生（required_elements 已确保），但保留兜底日志便于排错。 */
        LOGW("pag_branch: element 'pag0' missing in on_attached_locked");
        return false;
    }

    g_object_set(pag0, "pag-type", to_string(type_), nullptr);

    /* 子类钩子：sticker 注 pos+scale；effect 注 replace-image-*；默认空。 */
    inject_type_specific_locked(pag0);

    if (pag_file_path_.empty()) {
        /* 显式写空也走一遍，保证幂等：pagfilter 内部把空串视为 nullptr。 */
        g_object_set(pag0, "pag-file", "", nullptr);
        LOGI("pag_branch: pag-file left empty, filter stays in passthrough");
        return true;
    }

    g_object_set(pag0, "pag-file", pag_file_path_.c_str(), nullptr);
    LOGI("pag_branch: injected type='{}' pag-file='{}'",
         to_string(type_), pag_file_path_);
    return true;
}

/* ───────────────────── 公共热控实现 ─────────────────── */

bool PagBranch::set_pag_file(const std::string& abs_path, std::string& err) {
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
    LOGI("pag_branch: hot-set pag-file='{}'", abs_path);
    return true;
}

bool PagBranch::set_text(int idx, const std::string& utf8, std::string& err) {
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
    LOGI("pag_branch: hot-set pag-text idx={} len={}", idx, utf8.size());
    return true;
}

PagBranch::StatusSnapshot PagBranch::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    StatusSnapshot s;
    /* element() 不增 ref，但我们在持锁期间访问，BranchBase 保证元素引用
     * 在 detach_locked_ 之前一直有效。 */
    GstElement* pag0 = const_cast<PagBranch*>(this)->element("pag0");
    if (!pag0) {
        s.attached = false;
        return s;
    }
    s.attached = true;
    gchar* path = nullptr;
    gchar* type_str = nullptr;
    gfloat px = 0.5f, py = 0.5f, ps = 1.0f;
    g_object_get(pag0,
                 "pag-file",                  &path,
                 "pag-type",                  &type_str,
                 "pag-pos-x",                 &px,
                 "pag-pos-y",                 &py,
                 "pag-scale",                 &ps,
                 "pag-replace-image-idx",     &s.replace_idx,
                 "pag-replace-image-every",   &s.replace_every,
                 nullptr);
    if (path) {
        s.pag_file = path;
        g_free(path);
    }
    if (type_str) {
        s.type = type_str;
        g_free(type_str);
    } else {
        s.type = "sticker";
    }
    s.pos_x = px;
    s.pos_y = py;
    s.scale = ps;
    return s;
}

void PagBranch::format_status(std::string& out) const {
    auto s = snapshot();
    out.append("pag_attached=").append(s.attached ? "true" : "false").append("\n");
    if (s.attached) {
        out.append("pag_type=").append(s.type).append("\n");
        out.append("pag_file=").append(s.pag_file).append("\n");
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", s.pos_x);
        out.append("pag_pos_x=").append(buf).append("\n");
        std::snprintf(buf, sizeof(buf), "%.3f", s.pos_y);
        out.append("pag_pos_y=").append(buf).append("\n");
        std::snprintf(buf, sizeof(buf), "%.3f", s.scale);
        out.append("pag_scale=").append(buf).append("\n");
        out.append("pag_replace_idx=").append(std::to_string(s.replace_idx)).append("\n");
        out.append("pag_replace_every=").append(std::to_string(s.replace_every)).append("\n");
    }
}