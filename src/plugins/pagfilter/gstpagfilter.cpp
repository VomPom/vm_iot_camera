//
// Created by vompom on 2026/06/18.
//
// @Description
//   见 gstpagfilter.h 顶部说明。
//
//   实现要点（Stage 3 收尾后）：
//     - 走 GstBaseTransform 子类标准模板：klass_init 注册 pad 模板与 vmethod，
//       instance_init 设默认 passthrough=TRUE + in_place=TRUE，
//       基类完全短路，buffer 原样透传，零拷贝零修改。
//     - 仅保留 set_caps 打印一次，便于启动期确认协商落点。
//     - GST_DEBUG_CATEGORY 名称固定为 "pagfilter"。
//
//     Stage 4 起将在这里接入 libpag 渲染：取消 passthrough、绑定 transform_ip、
//     在 set_caps 中创建 PAG 渲染上下文。
//

#include "gstpagfilter.h"

GST_DEBUG_CATEGORY_STATIC(gst_pagfilter_debug);
#define GST_CAT_DEFAULT gst_pagfilter_debug

/* 仍仅声明 I420。Stage 4 接入 libpag 渲染时若需扩 NV12/RGBA 再回来升级。 */
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


/* ─────────────────────── vmethod 实现 ─────────────────────── */

/* set_caps：基类已校验 incaps/outcaps 通过模板。
 * 当前阶段仅打印一次便于启动期确认协商落点；Stage 4 起会在这里创建
 * PAG 渲染上下文（PAGSurface / PAGPlayer），并缓存 GstVideoInfo。 */
static gboolean
gst_pagfilter_set_caps(GstBaseTransform* trans,
                       GstCaps*          incaps,
                       GstCaps*          outcaps) {
    GstPagFilter* self = GST_PAGFILTER(trans);

    gchar* in_str  = gst_caps_to_string(incaps);
    gchar* out_str = gst_caps_to_string(outcaps);
    GST_INFO_OBJECT(self,
                    "pagfilter: set_caps in=%s out=%s",
                    in_str, out_str);
    g_free(in_str);
    g_free(out_str);
    return TRUE;
}

/* ─────────────────────── 类 / 实例初始化 ─────────────────────── */

static void
gst_pagfilter_class_init(GstPagFilterClass* klass) {
    GstElementClass*       element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass* trans_class   = GST_BASE_TRANSFORM_CLASS(klass);

    gst_element_class_set_static_metadata(
        element_class,
        "PAG Filter",
        "Filter/Effect/Video",
        "vm_iot custom video filter (passthrough skeleton; libpag overlay in Stage 4+)",
        "vompom <vm_iot maintainers>");

    gst_element_class_add_static_pad_template(element_class, &s_sink_template);
    gst_element_class_add_static_pad_template(element_class, &s_src_template);

    trans_class->set_caps = GST_DEBUG_FUNCPTR(gst_pagfilter_set_caps);
}

static void
gst_pagfilter_init(GstPagFilter* self) {
    /* 默认行为：do-nothing pass-through。Stage 4 接入 libpag 渲染时
     * 再在 set_property / set_caps 中按需切到 transform_ip。 */
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
#define VM_IOT_PAGFILTER_VERSION "0.3.0"
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
                           "(passthrough skeleton; libpag overlay in Stage 4+)"),
        plugin_init,
        VM_IOT_PAGFILTER_VERSION,
        "LGPL",
        PAGFILTER_PACKAGE,
        PAGFILTER_PACKAGE_NAME,
        PAGFILTER_PACKAGE_ORIGIN);
    return ok != FALSE;
}