//
// Created by vompom on 2026/06/24.
//
// @Description
//   见 pag_sticker.h 顶部说明。
//

#include "pag_sticker.h"

#include <gst/gst.h>

#include "log.h"

void PagSticker::inject_type_specific_locked(GstElement* pag0) {
    /* 启动期 sticker 专属属性：position（归一化中心对齐）+ scale。
     * 调用时机由基类 PagBranch::on_attached_locked() 在 pag-type 之后、
     * pag-file 之前同步触发，pag0 仍处于 NULL/READY，可安全写。 */
    g_object_set(pag0,
                 "pag-pos-x", static_cast<gfloat>(pos_x_),
                 "pag-pos-y", static_cast<gfloat>(pos_y_),
                 "pag-scale", static_cast<gfloat>(scale_),
                 nullptr);
    LOGI("pag_sticker: injected pos=({:.3f},{:.3f}) scale={:.3f}",
         pos_x_, pos_y_, scale_);
}

bool PagSticker::set_position(float x, float y, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* pag0 = element("pag0");
    if (!pag0) {
        err = "not_attached";
        return false;
    }
    /* GObject 属性自身范围会限住 [-2,3]，这里不再重复 clamp。 */
    g_object_set(pag0,
                 "pag-pos-x", static_cast<gfloat>(x),
                 "pag-pos-y", static_cast<gfloat>(y),
                 nullptr);
    pos_x_ = x;
    pos_y_ = y;
    LOGI("pag_sticker: hot-set pag-pos=({:.3f},{:.3f})", x, y);
    return true;
}

bool PagSticker::set_scale(float scale, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* pag0 = element("pag0");
    if (!pag0) {
        err = "not_attached";
        return false;
    }
    if (!(scale > 0.0f)) {
        err = "invalid_scale";
        return false;
    }
    g_object_set(pag0, "pag-scale", static_cast<gfloat>(scale), nullptr);
    scale_ = scale;
    LOGI("pag_sticker: hot-set pag-scale={:.3f}", scale);
    return true;
}
