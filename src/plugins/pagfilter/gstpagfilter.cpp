//
// Created by vompom on 2026/06/18.
//
// @Description
//   见 gstpagfilter.h 顶部说明。
//
//   实现要点（Stage 2 起更新）：
//     - 走 GstBaseTransform 子类标准模板：klass_init 注册 pad 模板与 vmethod，
//       instance_init 设默认 passthrough=TRUE。
//     - GObject 属性 `invert` (G_TYPE_BOOLEAN, 默认 FALSE)：
//         · false → 调 gst_base_transform_set_passthrough(TRUE)，基类完全短路，
//                   行为等价于 Stage 1，零拷贝零开销；
//         · true  → 调 gst_base_transform_set_passthrough(FALSE)，启用
//                   transform_ip，对每帧 I420 做 y = 255 - y / u = 255 - u /
//                   v = 255 - v 的逐像素反相。
//       读写都走 g_atomic_int_*：set_property 在主线程 / 控制线程，
//       transform_ip 在 streaming 线程，原子读避免半字节撕裂。
//     - set_caps：除 Stage 1 的日志外，**缓存 GstVideoInfo 到实例**，
//       transform_ip 复用，不再每帧重新 parse caps。
//     - GST_DEBUG_CATEGORY 名称固定为 "pagfilter"。
//

#include "gstpagfilter.h"

#include <gst/video/video.h>

GST_DEBUG_CATEGORY_STATIC(gst_pagfilter_debug);
#define GST_CAT_DEFAULT gst_pagfilter_debug

/* Stage 2 仍仅声明 I420。后续若要支持 NV12 / RGBA 再扩，
 * 但要同步评估 transform_ip 的逐 plane 处理代码。 */
#define PAGFILTER_CAPS_STR \
    "video/x-raw, " \
    "format = (string) I420, " \
    "width  = (int) [ 1, 2147483647 ], " \
    "height = (int) [ 1, 2147483647 ], " \
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

G_DEFINE_TYPE(GstPagFilter, gst_pagfilter, GST_TYPE_BASE_TRANSFORM);
//            ↑类型名        ↑函数前缀        ↑父类型


/* ─────────────────────── GObject 属性 ─────────────────────── */

enum {
    PROP_0,        /* GObject 约定：第 0 个 ID 占位，从 1 开始编号 */
    PROP_INVERT,
    PROP_LAST
};

#define DEFAULT_INVERT FALSE

static void
gst_pagfilter_set_property(GObject*      object,
                           guint         prop_id,
                           const GValue* value,
                           GParamSpec*   pspec) {
    GstPagFilter*     self  = GST_PAGFILTER(object);
    GstBaseTransform* trans = GST_BASE_TRANSFORM(self);

    switch (prop_id) {
        case PROP_INVERT: {
            const gboolean nv = g_value_get_boolean(value);
            g_atomic_int_set(&self->invert_atomic, nv ? 1 : 0);
            /* invert=FALSE 时短路 BaseTransform，行为等同 Stage 1；
             * invert=TRUE  时走 transform_ip。set_passthrough 是线程安全的，
             * 可以在 PLAYING 状态动态切换。 */
            gst_base_transform_set_passthrough(trans, nv ? FALSE : TRUE);
            GST_INFO_OBJECT(self, "pagfilter: invert -> %s", nv ? "true" : "false");
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
        case PROP_INVERT:
            g_value_set_boolean(value,
                                g_atomic_int_get(&self->invert_atomic) ? TRUE : FALSE);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/* ─────────────────────── vmethod 实现 ─────────────────────── */

/* set_caps：基类已校验 incaps/outcaps 通过模板。
 * Stage 2 起把 GstVideoInfo 缓存到实例，transform_ip 直接用，
 * 避免每帧重新 parse caps。 */
static gboolean
gst_pagfilter_set_caps(GstBaseTransform* trans,
                       GstCaps*          incaps,
                       GstCaps*          outcaps) {
    GstPagFilter* self = GST_PAGFILTER(trans);

    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, incaps)) {
        GST_WARNING_OBJECT(self, "pagfilter: gst_video_info_from_caps failed");
        self->info_valid = FALSE;
        return FALSE;
    }
    self->in_info    = info;
    self->info_valid = TRUE;

    /* incaps == outcaps 是 BaseTransform 默认行为（一进一出同 caps）。
     * 这里仅打印一次，便于启动期确认协商落点。 */
    gchar* in_str  = gst_caps_to_string(incaps);
    gchar* out_str = gst_caps_to_string(outcaps);
    GST_INFO_OBJECT(self,
                    "pagfilter: set_caps in=%s out=%s (size=%dx%d, fmt=%s)",
                    in_str, out_str,
                    GST_VIDEO_INFO_WIDTH(&info),
                    GST_VIDEO_INFO_HEIGHT(&info),
                    GST_VIDEO_INFO_NAME(&info));
    g_free(in_str);
    g_free(out_str);
    return TRUE;
}

/* transform_ip：Stage 2 的核心——对 I420 三个 plane 做 c = 255 - c。
 *
 * 设计要点：
 *   1) GstVideoFrame 抽象封装了 plane 指针 / stride / width / height，
 *      务必走 gst_video_frame_map(GST_MAP_READWRITE) 而不是直接用
 *      gst_buffer_map——前者会按 plane 拆开，stride 取得也更安全。
 *   2) **不要假设 stride == width**：I420 的 stride 经常会被 4/16 字节对齐
 *      抬到比 width 大；只能逐行 memset/loop 处理 width 个字节，下一行用 stride 跳。
 *   3) U/V plane 在 I420 下分辨率是 width/2 × height/2，stride 也是各自的，
 *      用 GST_VIDEO_FRAME_PLANE_* 宏取，不要自己除 2。
 *   4) passthrough 是 set_property 切换的，理论上 invert=FALSE 不会进入这里；
 *      但作为防御性代码仍然加一道 atomic 检查，避免 set_property 与
 *      transform_ip 之间的竞争窗口里漏处理。
 */
static GstFlowReturn
gst_pagfilter_transform_ip(GstBaseTransform* trans, GstBuffer* buf) {
    GstPagFilter* self = GST_PAGFILTER(trans);

    /* 防御性：set_property 把 passthrough 切回 TRUE 之前，可能有最后一帧
     * 已经进入 transform_ip。此时 atomic 读到 0，直接放行不修改。 */
    if (!g_atomic_int_get(&self->invert_atomic)) {
        return GST_FLOW_OK;
    }

    if (G_UNLIKELY(!self->info_valid)) {
        GST_WARNING_OBJECT(self, "transform_ip without valid in_info, skip");
        return GST_FLOW_OK;
    }

    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &self->in_info, buf,
                             static_cast<GstMapFlags>(GST_MAP_READWRITE))) {
        GST_ELEMENT_ERROR(self, RESOURCE, READ,
                          ("gst_video_frame_map failed"), (NULL));
        return GST_FLOW_ERROR;
    }

    /* I420 = 3 plane：Y / U / V。逐 plane 逐行做 c = 255 - c。 */
    const guint n_planes = GST_VIDEO_FRAME_N_PLANES(&frame);
    for (guint p = 0; p < n_planes; ++p) {
        guint8* base   = static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, p));
        const gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, p);
        const gint w      = GST_VIDEO_FRAME_COMP_WIDTH(&frame, p);
        const gint h      = GST_VIDEO_FRAME_COMP_HEIGHT(&frame, p);
        for (gint y = 0; y < h; ++y) {
            guint8* row = base + y * stride;
            for (gint x = 0; x < w; ++x) {
                row[x] = static_cast<guint8>(255 - row[x]);
            }
        }
    }

    gst_video_frame_unmap(&frame);
    return GST_FLOW_OK;
}

/* ─────────────────────── 类 / 实例初始化 ─────────────────────── */

static void
gst_pagfilter_class_init(GstPagFilterClass* klass) {
    GObjectClass*          gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass*       element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass* trans_class   = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->set_property = gst_pagfilter_set_property;
    gobject_class->get_property = gst_pagfilter_get_property;

    /* invert：Stage 2 唯一对外属性。
     * - 取值 FALSE 时元素行为等价 identity（passthrough=TRUE），零开销；
     * - 取值 TRUE 时对每帧 I420 做颜色反相 c = 255 - c。
     * G_PARAM_CONSTRUCT 让默认值在 g_object_new 时自动赋上去，
     * 避免依赖 instance_init 的赋值顺序。 */
    g_object_class_install_property(
        gobject_class,
        PROP_INVERT,
        g_param_spec_boolean(
            "invert",
            "Invert",
            "If TRUE, perform per-pixel color inversion (255 - c) on each I420 plane.",
            DEFAULT_INVERT,
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                                     GST_PARAM_MUTABLE_PLAYING |
                                     G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(
        element_class,
        "PAG Filter",
        "Filter/Effect/Video",
        "vm_iot custom video filter "
        "(Stage 2: optional color inversion; Stage 3+ libpag overlay)",
        "vompom <vm_iot maintainers>");

    gst_element_class_add_static_pad_template(element_class, &s_sink_template);
    gst_element_class_add_static_pad_template(element_class, &s_src_template);

    trans_class->set_caps     = GST_DEBUG_FUNCPTR(gst_pagfilter_set_caps);
    trans_class->transform_ip = GST_DEBUG_FUNCPTR(gst_pagfilter_transform_ip);

    /* 显式声明 transform_ip_on_passthrough=FALSE：passthrough 模式下
     * 不要回调 transform_ip。BaseTransform 默认值就是 FALSE，这里写出来
     * 是为了明确意图——Stage 2 的设计依赖 passthrough 短路 transform_ip。 */
    trans_class->transform_ip_on_passthrough = FALSE;
}

static void
gst_pagfilter_init(GstPagFilter* self) {
    /* 默认行为与 Stage 1 一致：do-nothing pass-through。
     * 只有当外部 g_object_set(invert=TRUE) 时才切换到 transform_ip 路径。 */
    GstBaseTransform* trans = GST_BASE_TRANSFORM(self);
    gst_base_transform_set_passthrough(trans, TRUE);
    gst_base_transform_set_in_place(trans, TRUE);

    /* C POD 字段显式清零；invert 默认值由 PROP_INVERT 的 G_PARAM_CONSTRUCT
     * 在 g_object_new 调用 set_property 时再写一次，二者互不冲突。 */
    g_atomic_int_set(&self->invert_atomic, DEFAULT_INVERT ? 1 : 0);
    gst_video_info_init(&self->in_info);
    self->info_valid = FALSE;
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
#define VM_IOT_PAGFILTER_VERSION "0.2.0"
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
        const_cast<gchar*>("vm_iot custom video filter "
                           "(Stage 2: optional color inversion)"),
        plugin_init,
        VM_IOT_PAGFILTER_VERSION,
        "LGPL",
        PAGFILTER_PACKAGE,
        PAGFILTER_PACKAGE_NAME,
        PAGFILTER_PACKAGE_ORIGIN);
    return ok != FALSE;
}
