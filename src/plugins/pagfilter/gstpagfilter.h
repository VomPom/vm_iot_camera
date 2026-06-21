//
// Created by vompom on 2026/06/18.
//
// @Description
//   pagfilter：vm_iot 自研 GStreamer 滤镜元素。
//
//   Stage 3 收尾：去掉 Stage 2 的 invert 像素特效与相关运行时状态，
//   回归到 Stage 1 的极简骨架（do-nothing pass-through），
//   为 Stage 4 接入 libpag 渲染留出干净起点。
//

#ifndef VM_IOT_GSTPAGFILTER_H
#define VM_IOT_GSTPAGFILTER_H

#pragma once

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_PAGFILTER            (gst_pagfilter_get_type())
#define GST_PAGFILTER(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PAGFILTER, GstPagFilter))
#define GST_PAGFILTER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PAGFILTER, GstPagFilterClass))
#define GST_IS_PAGFILTER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PAGFILTER))
#define GST_IS_PAGFILTER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PAGFILTER))

typedef struct _GstPagFilter      GstPagFilter;
typedef struct _GstPagFilterClass GstPagFilterClass;

struct _GstPagFilter {
    GstBaseTransform parent;
    /* Stage 4 起会在这里累积 PAG 渲染上下文 / 配置缓存等运行时状态。
     * 字段必须是 C POD —— GObject 派生实例结构不能放 ctor/dtor，构造由
     * gst_pagfilter_init 显式完成、释放由（未来的）finalize 回调显式完成。 */
};

struct _GstPagFilterClass {
    GstBaseTransformClass parent_class;
};

GType gst_pagfilter_get_type(void);

G_END_DECLS

/* ──────────────────────── 项目内调用接口 ────────────────────────
 * pagfilter_register_static：
 *   把 "pagfilter" 元素注册到默认 plugin registry，rank = GST_RANK_NONE。
 *   必须在 gst_init() 之后、build pipeline 之前调用一次。
 *   重复调用安全：内部走 GStreamer registry 的"已存在则跳过"语义。
 *
 * 返回 true 表示注册成功（或本次进程已注册过）；false 仅在底层 GLib
 * 类型系统异常时出现，调用方应当作启动期硬错误对待。
 */
bool pagfilter_register_static();

#endif //VM_IOT_GSTPAGFILTER_H
