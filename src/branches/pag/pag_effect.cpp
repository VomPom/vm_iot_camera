//
// Created by vompom on 2026/06/24.
//
// @Description
//   见 pag_effect.h 顶部说明。
//

#include "pag_effect.h"

#include <gst/gst.h>

#include "log.h"

void PagEffect::inject_type_specific_locked(GstElement* pag0) {
    /* 当前 pageffect 启动期没有专属属性需要预置。
     * 输出一行日志，方便运维一眼看到当前是 effect 模式（基类已经写过 type，
     * 这里加补充上下文），并提示底层 pagfilter 尚未真正实现 effect 路径。 */
    (void)pag0;
    LOGW("pag_effect: attached in pageffect mode; "
         "underlying pagfilter still in placeholder state (passthrough)");
}

bool PagEffect::set_replace_image_idx(int idx, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* pag0 = element("pag0");
    if (!pag0) {
        err = "not_attached";
        return false;
    }
    g_object_set(pag0, "pag-replace-image-idx", idx, nullptr);
    LOGI("pag_effect: hot-set pag-replace-image-idx={}", idx);
    return true;
}

bool PagEffect::set_replace_image_every(int every, std::string& err) {
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
    LOGI("pag_effect: hot-set pag-replace-image-every={}", every);
    return true;
}
