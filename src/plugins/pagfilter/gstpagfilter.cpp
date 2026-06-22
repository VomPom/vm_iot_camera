//
// Created by vompom on 2026/06/18.
//
// @Description
//   见 gstpagfilter.h 顶部说明。
//
//   Stage 4.3 实现要点：
//     - 新增 GObject 属性 `pag-file`（string, NULL/READY 时可写）。
//       为空 → passthrough；非空 → 在 set_caps 中按 (width, height)
//       创建 pag_sdk::Engine，transform_ip 内每帧渲染并 alpha-blend。
//     - 仍是 GstBaseTransform 子类：
//         · pag_file_path 为空时 passthrough=TRUE，零开销；
//         · 命中 Engine 后 passthrough=FALSE 且 in_place=TRUE，原地修改。
//     - libpag 关闭分支（VM_IOT_ENABLE_LIBPAG=OFF，单测路径）：
//       Engine::Make 永远返 nullptr，元素自动退化为 passthrough。
//     - 热切换 / 运行时改 pag-file 留给 Stage 4.4。
//

#include "gstpagfilter.h"

#include "pag_sdk.h"
#include "pag_blend.h"
#include "log.h"

#include <gst/video/video.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

GST_DEBUG_CATEGORY_STATIC(gst_pagfilter_debug);
#define GST_CAT_DEFAULT gst_pagfilter_debug

/* 仍仅声明 I420。Stage 4.4 主管线接入时若需扩 NV12/RGBA 再回来升级。 */
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
};

G_DEFINE_TYPE(GstPagFilter, gst_pagfilter, GST_TYPE_BASE_TRANSFORM);

/* ─────────────────────── 内部小工具 ─────────────────────── */

/* 把实例上的 Engine + 像素缓存释放掉。可重复调用，状态写回 NULL。
 * set_caps 重协商、stop、finalize 都会走它。 */
static void
gst_pagfilter_release_engine(GstPagFilter* self) {
    if (self->engine != nullptr) {
        delete static_cast<pag_sdk::Engine*>(self->engine);
        self->engine = nullptr;
    }
    if (self->rgba_buf != nullptr) {
        delete static_cast<std::vector<uint8_t>*>(self->rgba_buf);
        self->rgba_buf = nullptr;
    }
    self->stream_start_pts = GST_CLOCK_TIME_NONE;
    self->frame_counter    = 0;
}

/* 是否处于「打算渲染」状态：用户给了非空 pag-file。
 * 真正能否渲染还要看 Engine::Make 是否成功（libpag 关闭分支永远失败）。 */
static gboolean
gst_pagfilter_wants_render(GstPagFilter* self) {
    return self->pag_file_path != nullptr && self->pag_file_path[0] != '\0';
}

/* ─────────────────────── vmethod 实现 ─────────────────────── */

/* set_caps：基类已校验 incaps/outcaps 通过模板。
 * Stage 4.3：缓存 GstVideoInfo + 按 pag-file 创建 Engine + 切 passthrough。 */
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

    /* 缓存 VideoInfo（new/delete 因为 GObject 实例结构是 C POD）。 */
    if (self->in_info == nullptr) {
        self->in_info = new GstVideoInfo();
    }
    *self->in_info = info;

    gchar* in_str  = gst_caps_to_string(incaps);
    gchar* out_str = gst_caps_to_string(outcaps);
    GST_INFO_OBJECT(self,
                    "pagfilter: set_caps in=%s out=%s pag-file='%s'",
                    in_str, out_str,
                    self->pag_file_path ? self->pag_file_path : "(null)");
    g_free(in_str);
    g_free(out_str);

    /* 任何重协商都释放旧 Engine，按新尺寸重建。 */
    gst_pagfilter_release_engine(self);

    if (!gst_pagfilter_wants_render(self)) {
        /* 没设 pag-file：维持 passthrough。 */
        gst_base_transform_set_passthrough(trans, TRUE);
        gst_base_transform_set_in_place(trans, TRUE);
        return TRUE;
    }

    /* Surface 尺寸必须偶数（与 pag_blend 的 2×2 块约束一致）。
     * GstVideoInfo 给的 width/height 来自上游 caps，正常都是偶数；
     * 出现奇数（极端 caps）时强制下取偶。 */
    const int sw = GST_VIDEO_INFO_WIDTH(&info)  & ~1;
    const int sh = GST_VIDEO_INFO_HEIGHT(&info) & ~1;

    std::unique_ptr<pag_sdk::Engine> eng =
        pag_sdk::Engine::Make(self->pag_file_path, sw, sh);
    if (!eng) {
        /* libpag 关闭、文件不存在、Surface 创建失败 ⇒ 退化为 passthrough。
         * 这是 Stage 4.3 故意选择的「不报错只降级」语义：单测环境
         * （VM_IOT_ENABLE_LIBPAG=OFF）能走通；运行环境出问题时 LOGW
         * 由 Engine::Make 内部已经打过了，这里再打一条 element 视角的提示。 */
        GST_WARNING_OBJECT(self,
                           "pagfilter: Engine::Make('%s', %dx%d) failed, "
                           "falling back to passthrough",
                           self->pag_file_path, sw, sh);
        gst_base_transform_set_passthrough(trans, TRUE);
        gst_base_transform_set_in_place(trans, TRUE);
        return TRUE;
    }

    self->engine = eng.release();
    /* RGBA 缓冲一次性按 surface 尺寸分配，行距 = sw*4。 */
    auto* buf = new std::vector<uint8_t>();
    buf->resize(static_cast<size_t>(sw) * static_cast<size_t>(sh) * 4u);
    self->rgba_buf = buf;
    self->stream_start_pts = GST_CLOCK_TIME_NONE;
    self->frame_counter    = 0;

    /* 渲染就绪：切到 in_place 但取消 passthrough。 */
    gst_base_transform_set_passthrough(trans, FALSE);
    gst_base_transform_set_in_place(trans, TRUE);

    GST_INFO_OBJECT(self,
                    "pagfilter: render enabled, surface=%dx%d", sw, sh);
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

/* transform_ip：原地修改。
 * 仅在「engine != nullptr」时才被调用——passthrough 模式 BaseTransform
 * 直接短路，不会进这里。 */
static GstFlowReturn
gst_pagfilter_transform_ip(GstBaseTransform* trans,
                           GstBuffer*        buf) {
    GstPagFilter* self = GST_PAGFILTER(trans);
    if (self->engine == nullptr || self->in_info == nullptr) {
        /* 双保险：理论上不可达。 */
        return GST_FLOW_OK;
    }

    auto* eng     = static_cast<pag_sdk::Engine*>(self->engine);
    auto* rgba_v  = static_cast<std::vector<uint8_t>*>(self->rgba_buf);
    const int sw  = eng->surface_width();
    const int sh  = eng->surface_height();
    const size_t row_bytes = static_cast<size_t>(sw) * 4u;

    /* 1) 渲染 PAG 帧到 self->rgba_buf。失败时单帧降级（不修改 dst）。 */
    const double prog = gst_pagfilter_pts_to_progress(self, buf);
    if (!eng->render_frame_rgba(prog, rgba_v->data(), row_bytes)) {
        GST_WARNING_OBJECT(self,
                           "pagfilter: render_frame_rgba failed, "
                           "passing buffer through this frame");
        return GST_FLOW_OK;
    }

    /* 2) 把 RGBA premul 帧 blend 到 I420 buffer。
     *    用 GstVideoFrame 拿到 plane 指针 + stride，避免手算 offset。 */
    GstVideoFrame vframe;
    if (!gst_video_frame_map(&vframe, self->in_info, buf,
                              static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_WRITE))) {
        GST_WARNING_OBJECT(self,
                           "pagfilter: gst_video_frame_map failed, "
                           "passing buffer through this frame");
        return GST_FLOW_OK;
    }

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

    /* 当前贴到左上角 (0,0)。Stage 5 加锚点/缩放再扩。 */
    if (!pag_blend::blend_rgba_premul_over_i420(src, dst, 0, 0)) {
        GST_WARNING_OBJECT(self,
                           "pagfilter: blend_rgba_premul_over_i420 rejected "
                           "params; passing buffer through this frame");
    }

    gst_video_frame_unmap(&vframe);
    return GST_FLOW_OK;
}

/* ─────────────────────── GObject 属性 ─────────────────────── */

static void
gst_pagfilter_set_property(GObject*      object,
                           guint         prop_id,
                           const GValue* value,
                           GParamSpec*   pspec) {
    GstPagFilter* self = GST_PAGFILTER(object);
    switch (prop_id) {
    case PROP_PAG_FILE: {
        /* 与 Stage 4.3 锁定一致：仅 NULL / READY 状态允许改。
         * PAUSED/PLAYING 改属性会被静默拒绝（同时 LOGW），不影响管线。 */
        GstState st = GST_STATE_NULL;
        gst_element_get_state(GST_ELEMENT_CAST(self), &st, nullptr, 0);
        if (st > GST_STATE_READY) {
            GST_WARNING_OBJECT(self,
                               "pagfilter: pag-file is read/write only in "
                               "NULL/READY (current=%s); ignoring",
                               gst_element_state_get_name(st));
            return;
        }
        g_free(self->pag_file_path);
        const gchar* s = g_value_get_string(value);
        self->pag_file_path = (s && s[0] != '\0') ? g_strdup(s) : nullptr;
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
    switch (prop_id) {
    case PROP_PAG_FILE:
        g_value_set_string(value, self->pag_file_path ? self->pag_file_path : "");
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
gst_pagfilter_finalize(GObject* object) {
    GstPagFilter* self = GST_PAGFILTER(object);
    gst_pagfilter_release_engine(self);
    if (self->in_info != nullptr) {
        delete self->in_info;
        self->in_info = nullptr;
    }
    g_free(self->pag_file_path);
    self->pag_file_path = nullptr;
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
            "Writable only in NULL/READY state in Stage 4.3.",
            "",
            static_cast<GParamFlags>(
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

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
#define VM_IOT_PAGFILTER_VERSION "0.4.3"
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