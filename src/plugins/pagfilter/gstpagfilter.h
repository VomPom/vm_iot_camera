//
// Created by vompom on 2026/06/18.
//
// @Description
//   pagfilter：vm_iot 自研 GStreamer 滤镜元素。
//
//   Stage 4.3：接入 libpag 渲染。
//     - 新增 GObject 属性 `pag-file` (string, NULL/READY 时可写)：
//       为空字符串或 NULL 时元素保持 passthrough；非空时启动期在 set_caps
//       中按 (width, height) 创建 pag_sdk::Engine，transform_ip 内每帧
//       渲染 RGBA premul 帧并 alpha-blend 到 I420 buffer 上。
//     - libpag 真集成时（VM_IOT_ENABLE_LIBPAG=ON）才真正渲染；
//       VM_IOT_ENABLE_LIBPAG=OFF 时 Engine::Make 永远返 nullptr，
//       元素自动退化回 passthrough（单测路径走这里）。
//     - 热切换 / 运行时改 pag-file 留给 Stage 4.4。
//

#ifndef VM_IOT_GSTPAGFILTER_H
#define VM_IOT_GSTPAGFILTER_H

#pragma once

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video-info.h>

G_BEGIN_DECLS

#define GST_TYPE_PAGFILTER            (gst_pagfilter_get_type())
#define GST_PAGFILTER(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PAGFILTER, GstPagFilter))
#define GST_PAGFILTER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PAGFILTER, GstPagFilterClass))
#define GST_IS_PAGFILTER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PAGFILTER))
#define GST_IS_PAGFILTER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PAGFILTER))

typedef struct _GstPagFilter      GstPagFilter;
typedef struct _GstPagFilterClass GstPagFilterClass;

/* 实例结构必须是 C POD：所有 C++ 对象通过裸指针持有，在 init 中置 NULL、
 * 在 finalize 中显式 delete。
 *
 * Engine / rgba_buf 在 .cpp 内 #include "pag_sdk.h" 后会被前向声明引用——
 * 这里用 void* 避免把 C++ 头泄到本头文件。 */
struct _GstPagFilter {
    GstBaseTransform parent;

    /* ─── 配置（由 pag-file 属性写入，NULL/READY 时合法）─── */
    gchar*           pag_file_path;     /* g_strdup 出来，finalize 中 g_free */

    /* ─── 协商后缓存（set_caps 写、transform_ip 读，单线程）─── */
    GstVideoInfo*    in_info;           /* new/delete；NULL 表示尚未协商 */

    /* ─── 渲染状态（仅 streaming 线程访问）─── */
    void*            engine;            /* pag_sdk::Engine*；NULL 表示 passthrough */
    void*            rgba_buf;          /* std::vector<uint8_t>*；engine 命中时分配 */

    /* ─── PTS → progress 推进 ─── */
    GstClockTime     stream_start_pts;  /* 首帧 PTS；GST_CLOCK_TIME_NONE 表示未定 */
    guint64          frame_counter;     /* PTS 不可用时的退化路径计数器 */
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
