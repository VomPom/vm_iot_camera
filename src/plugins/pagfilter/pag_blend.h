//
// Created by vompom on 2026/06/21.
//
// @Description
//   Stage 4.2：把一帧 RGBA（premultiplied alpha）画面合成到一帧 I420 上的
//   纯算法模块。
//
//   设计动机：
//     - blend 跟 GstBuffer / stride / VideoInfo 强相关，但与 libpag / GStreamer
//       的对象生命周期无关。把它独立出来后：
//         · 单测可在 host 上裸调，不需要 gst_init / pag_sdk；
//         · gstpagfilter（Stage 4.3）只负责拿到 plane 指针 + 行距 + RGBA 缓冲，
//           直接调一次 blend_rgba_premul_over_i420(...) 即可。
//
//   像素格式契约：
//     - src： RGBA8888，分量顺序 R, G, B, A（小端字节序内存布局 0=R,1=G,2=B,3=A）；
//             **alpha 已 premultiplied**（即 R/G/B 已经预乘 A/255），与
//             pag_sdk::Engine::render_frame_rgba 的输出契约一致；
//     - dst： I420（YUV 4:2:0 平面布局），三个 plane 单独传指针 + stride；
//             色彩空间假定 **BT.601 limited range**（与 V4L2 USB camera /
//             GStreamer 默认输出一致）。其它色彩空间（BT.709 / full range）
//             需要换转换系数，目前不支持。
//
//   Blend 公式（Porter–Duff "src over dst"，src 已 premul）：
//     - RGB 域：out_rgb = src_rgb_premul + dst_rgb * (1 - src_a)
//       由于 dst (I420) 视作不透明（隐含 a=1），无需再算 out_a。
//     - 转换到 I420：把 out_rgb 用 BT.601 limited 公式重新算 Y / U / V，
//       覆盖 dst plane。U / V 采用 2×2 块平均：在 2×2 块内先做 RGB 域 blend，
//       再对 2×2 块的混合后 RGB 平均，再算 U/V。
//
//   位置 / 裁剪：
//     - 把 src 矩形 (src_w × src_h) 贴到 dst 的 (dst_x, dst_y) 处；
//     - 超出 dst 的部分自动裁剪；
//     - 因为 I420 的 U/V plane 是 2×2 子采样，dst_x 与 dst_y 必须为偶数，
//       否则函数返回 false 不做任何修改（避免色度撕裂）。
//
//   线程模型：
//     - 纯函数，无全局状态；多线程并发 blend 到不同 dst 区域是安全的。
//
//   性能：
//     - 标量 C++ 实现，720p × 720p 全覆盖在树莓派 4B aarch64 单核上预计
//       2–4 ms / 帧（30 fps 占用 6–12% 单核）。SIMD 优化留待 Stage 4.4
//       或必要时再做。
//

#ifndef VM_IOT_PAG_BLEND_H
#define VM_IOT_PAG_BLEND_H

#pragma once

#include <cstddef>
#include <cstdint>

namespace pag_blend {

/* I420 三 plane 的可写视图。所有 stride 单位均为字节。 */
struct I420Frame {
    uint8_t* y;          /* Y plane 起始（width × height）       */
    uint8_t* u;          /* U plane 起始（width/2 × height/2）   */
    uint8_t* v;          /* V plane 起始（width/2 × height/2）   */
    int      width;      /* 总宽度（像素）。必须为偶数            */
    int      height;     /* 总高度（像素）。必须为偶数            */
    int      y_stride;   /* Y plane 行距（字节，>= width）        */
    int      u_stride;   /* U plane 行距（字节，>= width/2）      */
    int      v_stride;   /* V plane 行距（字节，>= width/2）      */
};

/* RGBA premultiplied 源画面只读视图。 */
struct RgbaPremulFrame {
    const uint8_t* data;       /* 起始指针（R,G,B,A 字节顺序）        */
    int            width;      /* 宽度（像素）                        */
    int            height;     /* 高度（像素）                        */
    int            row_bytes;  /* 行距（字节，>= width × 4）          */
};

/* 把 src 贴到 dst 的 (dst_x, dst_y)，做 premul-over blend。
 *
 * 返回 true ：完成（即使裁剪后 0 像素也算成功）。
 * 返回 false：参数非法（指针为空 / stride 不足 / 尺寸不为偶数 /
 *             dst_x|dst_y 为奇数），dst 不做任何修改。
 *
 * 不抛异常，不打印日志（保留给调用方决定）。
 */
bool blend_rgba_premul_over_i420(const RgbaPremulFrame& src,
                                 const I420Frame&       dst,
                                 int                    dst_x,
                                 int                    dst_y);

/* ─────────────────────── 暴露给单测的颜色空间转换 ──────────────────────
 * 公开这些 helper 不是为生产代码——是为单测可独立验证 RGB ↔ YUV 的实现，
 * 避免在 blend 用例里同时调试 blend 公式与色彩公式两个变量。
 * 实现走 BT.601 limited range（Y∈[16,235], U/V∈[16,240]）。
 */
uint8_t rgb_to_y_bt601(uint8_t r, uint8_t g, uint8_t b);
uint8_t rgb_to_u_bt601(uint8_t r, uint8_t g, uint8_t b);
uint8_t rgb_to_v_bt601(uint8_t r, uint8_t g, uint8_t b);

}  // namespace pag_blend

#endif  // VM_IOT_PAG_BLEND_H
