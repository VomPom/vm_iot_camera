//
// Created by vompom on 2026/06/18.
//
// @Description
//   pagfilter：vm_iot 自研 GStreamer 滤镜元素。
//
//   能力要点：
//     - GObject 属性 `pag-file` (string)：
//       为空字符串或 NULL 时元素保持 passthrough；非空时在 set_caps
//       中按 (width, height) 创建 pag_sdk::Engine，transform_ip 内每帧
//       渲染 RGBA premul 帧并 alpha-blend 到 I420 buffer 上。
//     - libpag 真集成时（VM_IOT_ENABLE_LIBPAG=ON）才真正渲染；
//       VM_IOT_ENABLE_LIBPAG=OFF 时 Engine::Make 永远返 nullptr，
//       元素自动退化回 passthrough（单测路径走这里）。
//     - 支持 PLAYING 状态热切：运行期改 pag-file / 文本图层 / image placeholder
//       均通过 GObject 属性入队，由 streaming 线程下一帧消费。
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

    /* ─── 配置（由 GObject 属性写入；PLAYING 也可写）─── */
    gchar*           pag_file_path;     /* g_strdup 出来，finalize 中 g_free */

    /* ─── 协商后缓存（set_caps 写、transform_ip 读）─── */
    GstVideoInfo*    in_info;           /* new/delete；NULL 表示尚未协商 */

    /* ─── 渲染状态（仅 streaming 线程访问；属性写入侧靠 engine_lock_ 保护）─── */
    void*            engine;            /* pag_sdk::Engine*；NULL 表示 passthrough */
    void*            rgba_buf;          /* std::vector<uint8_t>*；engine 命中时分配 */

    /* ─── PTS → progress 推进 ─── */
    GstClockTime     stream_start_pts;  /* 首帧 PTS；GST_CLOCK_TIME_NONE 表示未定 */
    guint64          frame_counter;     /* PTS 不可用时的退化路径计数器 */

    /* ─── 热切 / 图层替换 队列 ───
     * 所有"控制线程发起、streaming 线程消费"的更新都走 pending_* 字段，
     * 由 engine_lock_ (GMutex*) 保护。streaming 线程每帧入口检查标志位、
     * 批量消费、清零。同步开销极小（无竞争路径下仅一次 mutex_trylock）。 */
    void*            engine_lock;       /* GMutex*；init 时分配，finalize 释放 */

    /* pag-file 热切：pending_pag_file 非空时 streaming 线程会按它重建 Engine。 */
    gboolean         reload_pending;
    gchar*           pending_pag_file;  /* g_strdup；消费后 g_free 并置 NULL */

    /* pag-text 热切：text_idx>=0 时 streaming 线程下一帧 replace_text。 */
    gint             pending_text_idx;
    gchar*           pending_text_utf8; /* g_strdup；消费后 g_free 并置 NULL */
    gboolean         text_pending;

    /* pag-replace-image：把当前 I420 帧转 RGBA 灌进第 N 个 image placeholder。
     * idx<0 → 不替换；every>=1 → 每 every 帧才替换一次（节流），减少
     * libpag 内部纹理重建开销。 */
    gint             replace_image_idx;
    gint             replace_image_every;
    guint64          replace_image_counter; /* 模 every 推进 */

    /* ─── 业务分路（type）与几何参数 ───
     *
     * type：sticker | pageffect。Sticker 走现有渲染+blend 路径；PagEffect
     *       是"视频替换轨道"占坑分支，transform_ip 顶部 passthrough+一次性 WARN。
     *       属性 flag = MUTABLE_READY：不允许 PLAYING 状态切，避免运行期
     *       重组渲染拓扑。
     *
     * pos_x / pos_y：sticker 模式下，PAG 画布中心对齐到画面归一化坐标。
     *                (0,0) 左上，(1,1) 右下。属性 flag = MUTABLE_PLAYING。
     *
     * pag_scale：sticker 模式下，PAG 渲染结果在 blend 前的等比缩放。
     *            1.0=与画面同尺寸；< 1.0 缩小；> 1.0 放大并自动裁剪。
     *            属性 flag = MUTABLE_PLAYING。
     *
     * 全部受 engine_lock 保护——属性 setter 写入、transform_ip 入口快照读取。 */
    gint             type;          /* 0=Sticker, 1=PagEffect；存 int 便于 g_value_get_int 兼容 */
    gfloat           pos_x;
    gfloat           pos_y;
    gfloat           pag_scale;
    gboolean         pageffect_warned; /* 仅在 PagEffect 模式下打一次 WARN，再往后静默 */
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
