//
// Created by vompom on 2026/06/18.
//
// @Description
//   见 gstpagfilter.h 顶部说明。
//
//   实现要点：
//     - `pag-file` 为 GST_PARAM_MUTABLE_PLAYING：PLAYING 状态也能改，
//       变更不会立即重建 Engine，而是写到 pending_pag_file_ + reload_pending
//       由 streaming 线程下一帧拿 engine_lock 安全消费（避免 set_caps /
//       transform_ip / set_property 三个线程互相打架）。
//     - 新增 `pag-text` 属性："idx:utf8" 格式；写入时缓存到 pending_text_*，
//       streaming 线程下一帧调 Engine::replace_text。
//     - 新增 `pag-replace-image-idx` / `pag-replace-image-every`：>=0 表示
//       要把当前 I420 帧灌入第 N 个 image placeholder。every 节流避免每帧
//       触发 libpag 纹理重建。
//
//   线程模型：
//     - 配置/控制线程：set_property → 拿 engine_lock_ → 仅写 pending_*
//     - streaming 线程：transform_ip 入口 → 拿 engine_lock_ → 消费 pending_*
//       （rebuild Engine / replace_text / replace_image）→ 释放 → 真正渲染
//     pending 字段只在持锁期间访问；Engine 本身仅 streaming 线程使用。
//

#include "gstpagfilter.h"

#include "pag_sdk.h"
#include "pag_blend.h"
#include "log.h"

#include <gst/video/video.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

GST_DEBUG_CATEGORY_STATIC(gst_pagfilter_debug);
#define GST_CAT_DEFAULT gst_pagfilter_debug

/* 仅声明 I420。未来若需扩 NV12/RGBA 再回来升级。 */
#define PAGFILTER_CAPS_STR \
    "video/x-raw, " \
    "format = (string) I420, " \
    "width  = (int) [ 2, 2147483647 ], " \
    "height = (int) [ 2, 2147483647 ], " \
    "framerate = (fraction) [ 0/1, 2147483647/1 ]"

static GstStaticPadTemplate s_sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(PAGFILTER_CAPS_STR));

static GstStaticPadTemplate s_src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(PAGFILTER_CAPS_STR));

enum {
    PROP_0,
    PROP_PAG_FILE,
    PROP_PAG_TEXT,
    PROP_REPLACE_IMAGE_IDX,
    PROP_REPLACE_IMAGE_EVERY,
};

G_DEFINE_TYPE(GstPagFilter, gst_pagfilter, GST_TYPE_BASE_TRANSFORM);

/* ─────────────────────── 内部小工具 ─────────────────────── */

/* 把实例上的 Engine + 像素缓存释放掉。可重复调用，状态写回 NULL。
 * **调用方必须持 engine_lock_**（如果 lock 已分配的话）。
 * set_caps 重协商、stop、finalize、热切流程都会走它。 */
static void
gst_pagfilter_release_engine_locked(GstPagFilter* self) {
    if (self->engine != nullptr) {
        delete static_cast<pag_sdk::Engine*>(self->engine);
        self->engine = nullptr;
    }
    if (self->rgba_buf != nullptr) {
        delete static_cast<std::vector<uint8_t>*>(self->rgba_buf);
        self->rgba_buf = nullptr;
    }
    self->stream_start_pts    = GST_CLOCK_TIME_NONE;
    self->frame_counter       = 0;
    self->replace_image_counter = 0;
}

/* 不持锁版本：早期生命周期（init/finalize/stop）使用，调用时刚分配/即将释放
 * engine_lock_，无并发风险。 */
static void
gst_pagfilter_release_engine(GstPagFilter* self) {
    GMutex* lk = static_cast<GMutex*>(self->engine_lock);
    if (lk) g_mutex_lock(lk);
    gst_pagfilter_release_engine_locked(self);
    if (lk) g_mutex_unlock(lk);
}

/* 按当前 in_info + pag_file_path 重建 Engine（streaming 线程持锁调用）。
 * 失败时保持 engine==nullptr，pagfilter 自然退化为 passthrough。 */
static void
gst_pagfilter_rebuild_engine_locked(GstPagFilter* self) {
    gst_pagfilter_release_engine_locked(self);

    if (self->pag_file_path == nullptr || self->pag_file_path[0] == '\0') {
        /* 空路径 = 显式退化为 passthrough。仍由 transform_ip 入口的
         * "engine==nullptr 短路"分支处理。 */
        return;
    }
    if (self->in_info == nullptr) {
        /* 尚未协商：等 set_caps 触发首次构造；保持 engine==nullptr 即可。 */
        return;
    }

    const int sw = GST_VIDEO_INFO_WIDTH(self->in_info)  & ~1;
    const int sh = GST_VIDEO_INFO_HEIGHT(self->in_info) & ~1;

    std::unique_ptr<pag_sdk::Engine> eng =
        pag_sdk::Engine::Make(self->pag_file_path, sw, sh);
    if (!eng) {
        GST_WARNING_OBJECT(self,
                           "pagfilter: Engine::Make('%s', %dx%d) failed, "
                           "falling back to passthrough",
                           self->pag_file_path, sw, sh);
        return;
    }

    self->engine = eng.release();
    auto* buf = new std::vector<uint8_t>();
    buf->resize(static_cast<size_t>(sw) * static_cast<size_t>(sh) * 4u);
    self->rgba_buf = buf;

    GST_INFO_OBJECT(self,
                    "pagfilter: engine rebuilt for '%s', surface=%dx%d "
                    "(numTexts=%d numImages=%d)",
                    self->pag_file_path, sw, sh,
                    static_cast<pag_sdk::Engine*>(self->engine)->num_texts(),
                    static_cast<pag_sdk::Engine*>(self->engine)->num_images());
}

/* I420 → RGBA8888 整帧转换（BT.601 limited range）。
 * 仅供 pag-replace-image 路径使用：每 N 帧 1 次，N 默认 2 → 30fps 时
 * ~15 次/秒，整帧转换在 aarch64 上 ~3-5ms 量级；如果用户把 every 调到 1
 * 才可能成为热点。
 *
 * 复用 pag_blend.cpp 暴露的 RGB → Y/U/V helper 不行（方向反了），这里
 * 自带一份反向公式：
 *   C = Y - 16,  D = U - 128,  E = V - 128
 *   R = clamp((298*C         + 409*E + 128) >> 8)
 *   G = clamp((298*C - 100*D - 208*E + 128) >> 8)
 *   B = clamp((298*C + 516*D         + 128) >> 8)
 *
 * 写入到 dst[height][row_bytes]，每像素 RGBA 4 字节，A 固定 0xFF。 */
static void
i420_to_rgba_bt601(const GstVideoFrame* vf,
                   uint8_t*             dst,
                   size_t               dst_row_bytes) {
    const int w  = GST_VIDEO_FRAME_WIDTH(vf)  & ~1;
    const int h  = GST_VIDEO_FRAME_HEIGHT(vf) & ~1;
    const int ys = GST_VIDEO_FRAME_PLANE_STRIDE(vf, 0);
    const int us = GST_VIDEO_FRAME_PLANE_STRIDE(vf, 1);
    const int vs = GST_VIDEO_FRAME_PLANE_STRIDE(vf, 2);
    const uint8_t* yp = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(vf, 0));
    const uint8_t* up = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(vf, 1));
    const uint8_t* vp = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(vf, 2));

    auto clamp_u8 = [](int v) -> uint8_t {
        if (v < 0)   return 0;
        if (v > 255) return 255;
        return static_cast<uint8_t>(v);
    };

    for (int y = 0; y < h; ++y) {
        const uint8_t* y_row = yp + y * ys;
        const uint8_t* u_row = up + (y >> 1) * us;
        const uint8_t* v_row = vp + (y >> 1) * vs;
        uint8_t*       d_row = dst + y * dst_row_bytes;
        for (int x = 0; x < w; ++x) {
            const int C = static_cast<int>(y_row[x]) - 16;
            const int D = static_cast<int>(u_row[x >> 1]) - 128;
            const int E = static_cast<int>(v_row[x >> 1]) - 128;
            d_row[x * 4 + 0] = clamp_u8((298 * C            + 409 * E + 128) >> 8);
            d_row[x * 4 + 1] = clamp_u8((298 * C - 100 * D  - 208 * E + 128) >> 8);
            d_row[x * 4 + 2] = clamp_u8((298 * C + 516 * D            + 128) >> 8);
            d_row[x * 4 + 3] = 0xFF;
        }
    }
}

/* ─────────────────────── vmethod 实现 ─────────────────────── */

/* set_caps：基类已校验 incaps/outcaps 通过模板。
 * 缓存 GstVideoInfo + 按 pag-file 创建 Engine + 切 passthrough，
 * 完成后均走 rebuild_engine_locked 重建 Engine。 */
static gboolean
gst_pagfilter_set_caps(GstBaseTransform* trans,
                       GstCaps*          incaps,
                       GstCaps*          outcaps) {
    GstPagFilter* self = GST_PAGFILTER(trans);

    /* 解析 caps：失败直接拒绝，让上游协商失败，比沉默退化更可调试。 */
    GstVideoInfo info;
    gst_video_info_init(&info);
    if (!gst_video_info_from_caps(&info, incaps)) {
        GST_ERROR_OBJECT(self, "pagfilter: gst_video_info_from_caps failed");
        return FALSE;
    }
    if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_I420) {
        GST_ERROR_OBJECT(self, "pagfilter: only I420 supported, got %s",
                         GST_VIDEO_INFO_NAME(&info));
        return FALSE;
    }

    gchar* in_str  = gst_caps_to_string(incaps);
    gchar* out_str = gst_caps_to_string(outcaps);
    GST_INFO_OBJECT(self,
                    "pagfilter: set_caps in=%s out=%s pag-file='%s'",
                    in_str, out_str,
                    self->pag_file_path ? self->pag_file_path : "(null)");
    g_free(in_str);
    g_free(out_str);

    GMutex* lk = static_cast<GMutex*>(self->engine_lock);
    g_mutex_lock(lk);

    /* 缓存 VideoInfo（new/delete 因为 GObject 实例结构是 C POD）。
     * 在锁内更新：rebuild_engine_locked 会读 in_info。 */
    if (self->in_info == nullptr) {
        self->in_info = new GstVideoInfo();
    }
    *self->in_info = info;

    /* 任何重协商都按当前 pag_file_path 重建 Engine（空路径 = 维持 passthrough）。 */
    gst_pagfilter_rebuild_engine_locked(self);

    const bool has_engine = (self->engine != nullptr);
    g_mutex_unlock(lk);

    if (has_engine) {
        gst_base_transform_set_passthrough(trans, FALSE);
        gst_base_transform_set_in_place(trans, TRUE);
        GST_INFO_OBJECT(self, "pagfilter: render enabled after caps negotiation");
    } else {
        /* 没设 pag-file 或 Engine::Make 失败：维持 passthrough。
 * 关键点：即便 PLAYING 后续 set_property("pag-file") 改了
         * 路径，下一帧 transform_ip 也走不进来（passthrough 模式 BaseTransform
         * 直接短路）。所以热切的 reload_pending 消费点必须放在 set_property
         * 触发 set_passthrough(FALSE) 的同时——见 set_property 实现。 */
        gst_base_transform_set_passthrough(trans, TRUE);
        gst_base_transform_set_in_place(trans, TRUE);
    }
    return TRUE;
}

/* stop：状态切回 READY 前的钩子。释放 Engine 与缓存。
 * finalize 也会兜底，但这里早释放可以让下次 NULL→READY 走干净路径。 */
static gboolean
gst_pagfilter_stop(GstBaseTransform* trans) {
    GstPagFilter* self = GST_PAGFILTER(trans);
    gst_pagfilter_release_engine(self);
    return TRUE;
}

/* 把 buffer PTS 转为 [0,1) 的 progress。
 * - duration_us == 0：静态 PAG，始终给 0；
 * - PTS 无效：用 frame_counter / 30.0 / (duration_us/1e6) 退化推进。 */
static double
gst_pagfilter_pts_to_progress(GstPagFilter* self,
                              GstBuffer*    buf) {
    pag_sdk::Engine* eng = static_cast<pag_sdk::Engine*>(self->engine);
    const int64_t dur_us = eng->duration_us();
    if (dur_us <= 0) {
        return 0.0;
    }
    const GstClockTime pts = GST_BUFFER_PTS(buf);
    int64_t elapsed_us = 0;
    if (pts != GST_CLOCK_TIME_NONE) {
        if (self->stream_start_pts == GST_CLOCK_TIME_NONE) {
            self->stream_start_pts = pts;
        }
        /* GstClockTime 单位是 ns。 */
        elapsed_us = static_cast<int64_t>(
            (pts - self->stream_start_pts) / 1000ULL);
    } else {
        /* PTS 不可用：假定 30fps 推进。 */
        elapsed_us = static_cast<int64_t>(self->frame_counter) * 33333;
    }
    self->frame_counter++;
    /* 模 dur_us 后归一化到 [0,1)。 */
    int64_t mod = elapsed_us % dur_us;
    if (mod < 0) mod += dur_us;
    return static_cast<double>(mod) / static_cast<double>(dur_us);
}

/* 消费一切 pending 控制变更：reload_pending / text_pending。
 * 在 transform_ip 入口持锁调用。
 * **注意**：image 替换是「拿当前帧」灌进 PAG，必须在 GstVideoFrame map
 * 之后另算，不在本函数里处理。 */
static void
gst_pagfilter_consume_pending_locked(GstPagFilter* self) {
    /* 1) pag-file 热切：若有 pending 路径，替换 pag_file_path_ 并重建 Engine。 */
    if (self->reload_pending) {
        g_free(self->pag_file_path);
        self->pag_file_path = self->pending_pag_file;  /* 转移所有权 */
        self->pending_pag_file = nullptr;
        self->reload_pending   = FALSE;
        gst_pagfilter_rebuild_engine_locked(self);
        GST_INFO_OBJECT(self,
                        "pagfilter: hot-swapped pag-file to '%s' (engine=%s)",
                        self->pag_file_path ? self->pag_file_path : "(null)",
                        self->engine ? "ok" : "null-passthrough");
    }

    /* 2) pag-text 热切：仅 Engine 已就绪时才下发，否则丢弃（路径切换后
     * idx 含义可能变，强行 replay 旧 idx 不安全）。 */
    if (self->text_pending && self->engine) {
        auto* eng = static_cast<pag_sdk::Engine*>(self->engine);
        const std::string utf8 =
            self->pending_text_utf8 ? std::string(self->pending_text_utf8) : std::string();
        const bool ok = eng->replace_text(self->pending_text_idx, utf8);
        GST_INFO_OBJECT(self,
                        "pagfilter: replace_text(idx=%d, len=%zu) -> %s",
                        self->pending_text_idx, utf8.size(),
                        ok ? "ok" : "fail");
        g_free(self->pending_text_utf8);
        self->pending_text_utf8 = nullptr;
        self->pending_text_idx  = -1;
        self->text_pending      = FALSE;
    }
}

/* transform_ip：原地修改。
 * 仅在「engine != nullptr」时才被调用——passthrough 模式 BaseTransform
 * 直接短路，不会进这里。 */
static GstFlowReturn
gst_pagfilter_transform_ip(GstBaseTransform* trans,
                           GstBuffer*        buf) {
    GstPagFilter* self = GST_PAGFILTER(trans);

    GMutex* lk = static_cast<GMutex*>(self->engine_lock);
    g_mutex_lock(lk);

    /* 1) 先消费 pending：可能在此分支换文件 / 改文本，进而改变 engine。 */
    gst_pagfilter_consume_pending_locked(self);

    if (self->engine == nullptr || self->in_info == nullptr) {
        /* 双保险：理论上不可达（passthrough 模式不会进这里）。 */
        g_mutex_unlock(lk);
        return GST_FLOW_OK;
    }

    auto* eng     = static_cast<pag_sdk::Engine*>(self->engine);
    auto* rgba_v  = static_cast<std::vector<uint8_t>*>(self->rgba_buf);
    const int sw  = eng->surface_width();
    const int sh  = eng->surface_height();
    const size_t row_bytes = static_cast<size_t>(sw) * 4u;

    /* 2) 计算 progress + 准备 image replacement 节流计数。
     * 拿出本帧需要的所有标量后即可放锁——剩下的渲染/blend 完全访问
     * 局部缓冲与 eng（仅 streaming 线程）。 */
    const double prog        = gst_pagfilter_pts_to_progress(self, buf);
    const gint   img_idx     = self->replace_image_idx;
    const gint   img_every   = self->replace_image_every > 0 ? self->replace_image_every : 1;
    const bool   should_replace_img =
        (img_idx >= 0) &&
        ((self->replace_image_counter++ % static_cast<guint64>(img_every)) == 0);

    g_mutex_unlock(lk);

    /* 3) Map I420 buffer：image replacement 与 blend 都要用到 plane 视图。 */
    GstVideoFrame vframe;
    if (!gst_video_frame_map(&vframe, self->in_info, buf,
                              static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_WRITE))) {
        GST_WARNING_OBJECT(self,
                           "pagfilter: gst_video_frame_map failed, "
                           "passing buffer through this frame");
        return GST_FLOW_OK;
    }

    /* 4) 如果用户配了 pag-replace-image-idx，按节流频率把当前 I420 帧
     * 转 RGBA 灌进 PAG image layer。**必须在 render_frame_rgba 之前**
     * 调用，下一帧渲染才会生效。 */
    if (should_replace_img) {
        const int rgba_w = GST_VIDEO_FRAME_WIDTH(&vframe)  & ~1;
        const int rgba_h = GST_VIDEO_FRAME_HEIGHT(&vframe) & ~1;
        std::vector<uint8_t> rgba_camera(
            static_cast<size_t>(rgba_w) * static_cast<size_t>(rgba_h) * 4u);
        i420_to_rgba_bt601(&vframe, rgba_camera.data(),
                           static_cast<size_t>(rgba_w) * 4u);
        const bool ok = eng->replace_image_from_rgba(
            img_idx, rgba_camera.data(), rgba_w, rgba_h,
            static_cast<size_t>(rgba_w) * 4u);
        if (!ok) {
            GST_WARNING_OBJECT(self,
                               "pagfilter: replace_image_from_rgba(idx=%d) failed",
                               img_idx);
        }
    }

    /* 5) 渲染 PAG 帧到 self->rgba_buf。失败时单帧降级（不修改 dst）。 */
    if (!eng->render_frame_rgba(prog, rgba_v->data(), row_bytes)) {
        GST_WARNING_OBJECT(self,
                           "pagfilter: render_frame_rgba failed, "
                           "passing buffer through this frame");
        gst_video_frame_unmap(&vframe);
        return GST_FLOW_OK;
    }

    /* 6) 把 RGBA premul 帧 blend 到 I420 buffer。 */
    pag_blend::I420Frame dst;
    dst.y        = static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0));
    dst.u        = static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 1));
    dst.v        = static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 2));
    dst.width    = GST_VIDEO_FRAME_WIDTH(&vframe)  & ~1;
    dst.height   = GST_VIDEO_FRAME_HEIGHT(&vframe) & ~1;
    dst.y_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0);
    dst.u_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 1);
    dst.v_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 2);

    pag_blend::RgbaPremulFrame src;
    src.data      = rgba_v->data();
    src.width     = sw;
    src.height    = sh;
    src.row_bytes = static_cast<int>(row_bytes);

    /* 当前贴到左上角 (0,0)。锥点/缩放后置。 */
    if (!pag_blend::blend_rgba_premul_over_i420(src, dst, 0, 0)) {
        GST_WARNING_OBJECT(self,
                           "pagfilter: blend_rgba_premul_over_i420 rejected "
                           "params; passing buffer through this frame");
    }

    gst_video_frame_unmap(&vframe);
    return GST_FLOW_OK;
}

/* ─────────────────────── GObject 属性 ─────────────────────── */

/* 解析 "idx:utf8" 形式的 pag-text 入参，返回 (idx, utf8) 或 (-1, "")。
 * idx 必须为 0..1024 之间的整数；utf8 部分允许为空（即清空文本）。
 * 异常输入只返 idx=-1，外层会拒绝。 */
static void
parse_pag_text(const gchar* raw, gint* out_idx, gchar** out_utf8) {
    *out_idx  = -1;
    *out_utf8 = nullptr;
    if (!raw || !*raw) return;
    const char* colon = std::strchr(raw, ':');
    if (!colon) return;
    char* endp = nullptr;
    long idx = std::strtol(raw, &endp, 10);
    if (endp != colon) return;
    if (idx < 0 || idx > 1024) return;
    *out_idx  = static_cast<gint>(idx);
    *out_utf8 = g_strdup(colon + 1);
}

static void
gst_pagfilter_set_property(GObject*      object,
                           guint         prop_id,
                           const GValue* value,
                           GParamSpec*   pspec) {
    GstPagFilter* self = GST_PAGFILTER(object);
    GMutex* lk = static_cast<GMutex*>(self->engine_lock);

    switch (prop_id) {
    case PROP_PAG_FILE: {
    /* 允许 PLAYING 状态热切。逻辑分两段：
         *   - 状态 ≤ READY：直接更新 pag_file_path_（set_caps 还没跑或重跑时
         *     再按它构造 Engine）；
         *   - 状态 ≥ PAUSED：写入 pending_pag_file_ + reload_pending，
         *     streaming 线程下一帧 transform_ip 入口消费。
         * 这里同时调 set_passthrough(FALSE) 是为了万一上次因空路径退到
         * passthrough，下次 transform_ip 进不来；set_passthrough 是幂等且
         * 线程安全的（GstBaseTransform 内部用 g_atomic）。 */
        GstState st = GST_STATE_NULL;
        gst_element_get_state(GST_ELEMENT_CAST(self), &st, nullptr, 0);
        const gchar* s = g_value_get_string(value);
        gchar* new_path = (s && s[0] != '\0') ? g_strdup(s) : nullptr;

        g_mutex_lock(lk);
        if (st <= GST_STATE_READY) {
            /* 直接覆盖；后续 set_caps 会按它建 Engine。 */
            g_free(self->pag_file_path);
            self->pag_file_path = new_path;
            /* 同步清掉 pending（如果之前有 PLAYING 时写入但还没消费）。 */
            if (self->pending_pag_file) {
                g_free(self->pending_pag_file);
                self->pending_pag_file = nullptr;
            }
            self->reload_pending = FALSE;
        } else {
            /* PLAYING 等运行态：排队让 streaming 线程消费。 */
            if (self->pending_pag_file) {
                g_free(self->pending_pag_file);
            }
            self->pending_pag_file = new_path;  /* 转移所有权 */
            self->reload_pending   = TRUE;
            /* 强制取消 passthrough，让 transform_ip 能被调到以消费 pending。
             * 实际是否真渲染由 rebuild 后 engine 是否非空决定；rebuild 失败
             * 时下一次 transform_ip 入口会发现 engine==nullptr 直接退回。 */
            gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
            gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
        }
        g_mutex_unlock(lk);
        break;
    }
    case PROP_PAG_TEXT: {
        /* "idx:utf8" 格式；idx<0 表示主动清除（拒绝执行）。 */
        const gchar* raw = g_value_get_string(value);
        gint   idx = -1;
        gchar* utf8 = nullptr;
        parse_pag_text(raw, &idx, &utf8);
        if (idx < 0) {
            GST_WARNING_OBJECT(self,
                               "pagfilter: pag-text format invalid "
                               "(expected '<idx>:<utf8>'), got '%s'",
                               raw ? raw : "(null)");
            g_free(utf8);
            break;
        }
        g_mutex_lock(lk);
        if (self->pending_text_utf8) g_free(self->pending_text_utf8);
        self->pending_text_idx  = idx;
        self->pending_text_utf8 = utf8;  /* 转移所有权 */
        self->text_pending      = TRUE;
        g_mutex_unlock(lk);
        break;
    }
    case PROP_REPLACE_IMAGE_IDX: {
        g_mutex_lock(lk);
        self->replace_image_idx = g_value_get_int(value);
        self->replace_image_counter = 0;
        g_mutex_unlock(lk);
        break;
    }
    case PROP_REPLACE_IMAGE_EVERY: {
        gint v = g_value_get_int(value);
        if (v < 1) v = 1;
        g_mutex_lock(lk);
        self->replace_image_every = v;
        g_mutex_unlock(lk);
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
gst_pagfilter_get_property(GObject*    object,
                           guint       prop_id,
                           GValue*     value,
                           GParamSpec* pspec) {
    GstPagFilter* self = GST_PAGFILTER(object);
    GMutex* lk = static_cast<GMutex*>(self->engine_lock);
    switch (prop_id) {
    case PROP_PAG_FILE:
        g_mutex_lock(lk);
        /* 若有 pending 路径，优先返回它——使用者刚 set 完立刻 get 应能拿到。 */
        if (self->reload_pending && self->pending_pag_file) {
            g_value_set_string(value, self->pending_pag_file);
        } else {
            g_value_set_string(value, self->pag_file_path ? self->pag_file_path : "");
        }
        g_mutex_unlock(lk);
        break;
    case PROP_PAG_TEXT:
        /* 只读最后一次 set 的 raw（保留 idx:utf8 形式便于回执）。 */
        g_mutex_lock(lk);
        if (self->text_pending && self->pending_text_utf8) {
            gchar* combined = g_strdup_printf("%d:%s",
                                              self->pending_text_idx,
                                              self->pending_text_utf8);
            g_value_set_string(value, combined);
            g_free(combined);
        } else {
            g_value_set_string(value, "");
        }
        g_mutex_unlock(lk);
        break;
    case PROP_REPLACE_IMAGE_IDX:
        g_mutex_lock(lk);
        g_value_set_int(value, self->replace_image_idx);
        g_mutex_unlock(lk);
        break;
    case PROP_REPLACE_IMAGE_EVERY:
        g_mutex_lock(lk);
        g_value_set_int(value, self->replace_image_every);
        g_mutex_unlock(lk);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
gst_pagfilter_finalize(GObject* object) {
    GstPagFilter* self = GST_PAGFILTER(object);
    /* 释放 Engine（不持锁的版本，因为 lock 自己即将销毁；此刻已无其它
     * 线程访问本对象——GObject 在 ref==0 时调 finalize）。 */
    gst_pagfilter_release_engine(self);
    if (self->in_info != nullptr) {
        delete self->in_info;
        self->in_info = nullptr;
    }
    g_free(self->pag_file_path);
    self->pag_file_path = nullptr;
    if (self->pending_pag_file) {
        g_free(self->pending_pag_file);
        self->pending_pag_file = nullptr;
    }
    if (self->pending_text_utf8) {
        g_free(self->pending_text_utf8);
        self->pending_text_utf8 = nullptr;
    }
    if (self->engine_lock) {
        g_mutex_clear(static_cast<GMutex*>(self->engine_lock));
        g_free(self->engine_lock);
        self->engine_lock = nullptr;
    }
    G_OBJECT_CLASS(gst_pagfilter_parent_class)->finalize(object);
}

/* ─────────────────────── 类 / 实例初始化 ─────────────────────── */

static void
gst_pagfilter_class_init(GstPagFilterClass* klass) {
    GObjectClass*          gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass*       element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass* trans_class   = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_pagfilter_set_property);
    gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_pagfilter_get_property);
    gobject_class->finalize     = GST_DEBUG_FUNCPTR(gst_pagfilter_finalize);

    g_object_class_install_property(
        gobject_class,
        PROP_PAG_FILE,
        g_param_spec_string(
            "pag-file",
            "PAG file path",
            "Path to a .pag asset. Empty/NULL keeps element in passthrough. "
            "Writable in any state including PLAYING; hot-swap "
            "is queued and applied at next streaming-thread frame.",
            "",
            static_cast<GParamFlags>(
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                GST_PARAM_MUTABLE_PLAYING)));

    g_object_class_install_property(
        gobject_class,
        PROP_PAG_TEXT,
        g_param_spec_string(
            "pag-text",
            "PAG text replacement",
            "Replace a text layer. Format: '<idx>:<utf8>'. "
            "idx must be in [0, numTexts). Writable in any state; applied at "
            "next streaming-thread frame.",
            "",
            static_cast<GParamFlags>(
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                GST_PARAM_MUTABLE_PLAYING)));

    g_object_class_install_property(
        gobject_class,
        PROP_REPLACE_IMAGE_IDX,
        g_param_spec_int(
            "pag-replace-image-idx",
            "Image layer index to replace with live frame",
            "Replace the N-th PAG image placeholder with the current camera "
            "frame each cycle (see pag-replace-image-every). -1 disables.",
            -1, 1024, -1,
            static_cast<GParamFlags>(
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                GST_PARAM_MUTABLE_PLAYING)));

    g_object_class_install_property(
        gobject_class,
        PROP_REPLACE_IMAGE_EVERY,
        g_param_spec_int(
            "pag-replace-image-every",
            "Frame interval between image replacements",
            "Only call replaceImage once every N frames. Higher values reduce "
            "libpag texture-rebuild cost. Minimum 1 (every frame).",
            1, 60, 2,
            static_cast<GParamFlags>(
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                GST_PARAM_MUTABLE_PLAYING)));

    gst_element_class_set_static_metadata(
        element_class,
        "PAG Filter",
        "Filter/Effect/Video",
        "vm_iot custom video filter: render libpag asset and alpha-blend onto I420",
        "vompom <vm_iot maintainers>");

    gst_element_class_add_static_pad_template(element_class, &s_sink_template);
    gst_element_class_add_static_pad_template(element_class, &s_src_template);

    trans_class->set_caps     = GST_DEBUG_FUNCPTR(gst_pagfilter_set_caps);
    trans_class->stop         = GST_DEBUG_FUNCPTR(gst_pagfilter_stop);
    trans_class->transform_ip = GST_DEBUG_FUNCPTR(gst_pagfilter_transform_ip);
}

static void
gst_pagfilter_init(GstPagFilter* self) {
    /* 实例结构是 C POD，GObject 不会自动零初始化用户字段——显式置 NULL。 */
    self->pag_file_path    = nullptr;
    self->in_info          = nullptr;
    self->engine           = nullptr;
    self->rgba_buf         = nullptr;
    self->stream_start_pts = GST_CLOCK_TIME_NONE;
    self->frame_counter    = 0;

    /* 热切 / 图层替换 字段 */
    GMutex* lk = static_cast<GMutex*>(g_malloc0(sizeof(GMutex)));
    g_mutex_init(lk);
    self->engine_lock        = lk;
    self->reload_pending     = FALSE;
    self->pending_pag_file   = nullptr;
    self->pending_text_idx   = -1;
    self->pending_text_utf8  = nullptr;
    self->text_pending       = FALSE;
    self->replace_image_idx  = -1;
    self->replace_image_every = 2;
    self->replace_image_counter = 0;

    /* 默认 passthrough；set_caps 命中渲染时再切到非 passthrough。 */
    GstBaseTransform* trans = GST_BASE_TRANSFORM(self);
    gst_base_transform_set_passthrough(trans, TRUE);
    gst_base_transform_set_in_place(trans, TRUE);
}

/* ─────────────────────── plugin / 静态注册 ─────────────────────── */

static gboolean
plugin_init(GstPlugin* plugin) {
    GST_DEBUG_CATEGORY_INIT(gst_pagfilter_debug,
                            "pagfilter", 0,
                            "vm_iot pagfilter element");
    return gst_element_register(plugin,
                                "pagfilter",
                                GST_RANK_NONE,
                                GST_TYPE_PAGFILTER);
}

#ifndef VM_IOT_PAGFILTER_VERSION
#define VM_IOT_PAGFILTER_VERSION "0.5.0"
#endif

/* PACKAGE / GST_PACKAGE_NAME / GST_PACKAGE_ORIGIN 是 GST_PLUGIN_DEFINE 强制宏，
 * 单独 #define 一份，避免污染全局。 */
#define PAGFILTER_PACKAGE        "vm_iot"
#define PAGFILTER_PACKAGE_NAME   "vm_iot custom plugins"
#define PAGFILTER_PACKAGE_ORIGIN "https://github.com/vompom/vm_iot"

bool pagfilter_register_static() {
    /* gst_plugin_register_static 内部按 (major,minor,name) 去重，
     * 重复调用安全（返回 TRUE 但不会重新注册）。 */
    gboolean ok = gst_plugin_register_static(
        GST_VERSION_MAJOR,
        GST_VERSION_MINOR,
        "pagfilter",
        const_cast<gchar*>("vm_iot custom video filter: "
                           "render libpag asset and alpha-blend onto I420"),
        plugin_init,
        VM_IOT_PAGFILTER_VERSION,
        "LGPL",
        PAGFILTER_PACKAGE,
        PAGFILTER_PACKAGE_NAME,
        PAGFILTER_PACKAGE_ORIGIN);
    return ok != FALSE;
}