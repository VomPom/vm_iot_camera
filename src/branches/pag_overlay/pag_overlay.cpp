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
