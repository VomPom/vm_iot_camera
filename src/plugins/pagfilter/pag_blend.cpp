//
// Created by vompom on 2026/06/21.
//
// @Description
//   见 pag_blend.h 顶部说明。
//
//   实现要点：
//     - BT.601 limited range 整数定点公式（与 GStreamer videoconvert 默认一致）：
//         Y =  (( 66 * R + 129 * G +  25 * B + 128) >> 8) + 16
//         U =  ((-38 * R -  74 * G + 112 * B + 128) >> 8) + 128
//         V =  ((112 * R -  94 * G -  18 * B + 128) >> 8) + 128
//       系数来源：ITU-R BT.601 / Rec.601。这里采用最常见的 Wikipedia /
//       libyuv 同款 8-bit 定点版本，>>8 后加 128 / 16 偏移。
//
//     - premul-over blend：
//         out_rgb = src_rgb_premul + dst_rgb * (255 - src_a) / 255
//       使用 ((x + 128) * 257) >> 16 近似 x / 255，避免除法。这里直接
//       (255 - a) * c + 127) / 255 也行；为了写得直接易读，
//       选择 (val + 127) / 255 整数近似。
//
//     - U/V 采样：对每个 2×2 块，先把 4 个像素分别 blend 出 RGB，再对
//       4 个 RGB 求平均，最后算 U/V。Y 平面则逐像素独立写。
//
//   单元测试见 tests/test_pag_blend.cpp。
//

#include "pag_blend.h"

#include <algorithm>
#include <cstring>

namespace pag_blend {

namespace {

inline uint8_t clamp_u8(int v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

/* 用 (val + 127) / 255 做整数近似除法，等价于 round(val/255)。
 * 用于 dst 分量 × (255-a) 的归一。 */
inline int div255_round(int v) {
    return (v + 127) / 255;
}

/* premul-over：src 已 premul，dst 视作不透明。
 *   out = src + dst * (255 - a) / 255  （per channel）
 * 由于 src 已 premul，alpha 只用来算 dst 的衰减系数，不再乘 src。 */
inline void premul_over(uint8_t  src_r, uint8_t src_g, uint8_t src_b, uint8_t src_a,
                        uint8_t  dst_r, uint8_t dst_g, uint8_t dst_b,
                        uint8_t& out_r, uint8_t& out_g, uint8_t& out_b) {
    const int inv_a = 255 - static_cast<int>(src_a);
    out_r = clamp_u8(static_cast<int>(src_r) + div255_round(dst_r * inv_a));
    out_g = clamp_u8(static_cast<int>(src_g) + div255_round(dst_g * inv_a));
    out_b = clamp_u8(static_cast<int>(src_b) + div255_round(dst_b * inv_a));
}

/* BT.601 limited range：把当前 I420 像素 (y, u, v) 反算回 RGB。
 * 因为 U/V 是 4 个像素共享一个，反算时给每个像素都用 2×2 块共同的 u/v。
 *
 *   C = Y - 16
 *   D = U - 128
 *   E = V - 128
 *   R = clamp((298*C         + 409*E + 128) >> 8)
 *   G = clamp((298*C - 100*D - 208*E + 128) >> 8)
 *   B = clamp((298*C + 516*D         + 128) >> 8)
 *
 * 仅用于 U/V 平面写入时反算 dst RGB；Y 平面写入是逐像素的，可以独立处理。 */
inline void yuv_to_rgb_bt601(uint8_t y, uint8_t u, uint8_t v,
                             uint8_t& r, uint8_t& g, uint8_t& b) {
    const int C = static_cast<int>(y) - 16;
    const int D = static_cast<int>(u) - 128;
    const int E = static_cast<int>(v) - 128;
    r = clamp_u8((298 * C            + 409 * E + 128) >> 8);
    g = clamp_u8((298 * C - 100 * D  - 208 * E + 128) >> 8);
    b = clamp_u8((298 * C + 516 * D            + 128) >> 8);
}

}  // namespace

uint8_t rgb_to_y_bt601(uint8_t r, uint8_t g, uint8_t b) {
    const int y = (( 66 * r + 129 * g +  25 * b + 128) >> 8) + 16;
    return clamp_u8(y);
}

uint8_t rgb_to_u_bt601(uint8_t r, uint8_t g, uint8_t b) {
    const int u = ((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128;
    return clamp_u8(u);
}

uint8_t rgb_to_v_bt601(uint8_t r, uint8_t g, uint8_t b) {
    const int v = ((112 * r -  94 * g -  18 * b + 128) >> 8) + 128;
    return clamp_u8(v);
}

bool blend_rgba_premul_over_i420(const RgbaPremulFrame& src,
                                 const I420Frame&       dst,
                                 int                    dst_x,
                                 int                    dst_y) {
    /* ──────── 参数校验：发现非法立即返回，不做任何写入 ──────── */
    if (!src.data) return false;
    if (!dst.y || !dst.u || !dst.v) return false;

    if (src.width <= 0 || src.height <= 0) {
        /* 0 像素源：合法但 no-op。 */
        return true;
    }
    if (src.row_bytes < src.width * 4) return false;

    if (dst.width  <= 0 || dst.height <= 0) return false;
    if ((dst.width  & 1) != 0) return false;
    if ((dst.height & 1) != 0) return false;
    if (dst.y_stride < dst.width)        return false;
    if (dst.u_stride < dst.width / 2)    return false;
    if (dst.v_stride < dst.width / 2)    return false;

    /* I420 的 U/V plane 是 2×2 子采样，目标坐标必须偶数对齐。 */
    if ((dst_x & 1) != 0) return false;
    if ((dst_y & 1) != 0) return false;

    /* ──────── 裁剪：算出 src 在 dst 内的有效矩形 ──────── */
    int copy_x0 = std::max(0, -dst_x);                       /* src 起始 x 偏移 */
    int copy_y0 = std::max(0, -dst_y);                       /* src 起始 y 偏移 */
    int copy_w  = std::min(src.width,  dst.width  - dst_x) - copy_x0;
    int copy_h  = std::min(src.height, dst.height - dst_y) - copy_y0;
    if (copy_w <= 0 || copy_h <= 0) return true;             /* 完全在 dst 外，no-op */

    /* 子采样块对齐：U/V 是 2×2 块共享，所以 blend 区域要按 2×2 块为单位
     * 处理。把 copy 区域向下取整对齐到偶数尺寸；剩下的奇数行/列像素
     * 在 Y 平面正常写，但其 U/V 块如果跨越 src 边界，会用边界外像素
     * 视作 src.a = 0（即只保留 dst），等价于不动。
     *
     * 简化策略：把 copy 区域向下取整到偶数对齐，奇数行/列丢弃。这会在
     * src 宽/高为奇数时丢失最右/最下一行像素。当前 PAG 渲染
     * Surface 尺寸由我们自己控制（一般 ≤ 720×720 且取偶数），此简化
     * 不会触发；如未来需支持奇数 src，再扩展。 */
    int x0_src = copy_x0;
    int y0_src = copy_y0;
    int x0_dst = dst_x + copy_x0;
    int y0_dst = dst_y + copy_y0;

    /* 确保 (x0_dst, y0_dst) 偶数对齐：因为 dst_x/dst_y 已偶数，
     * copy_x0/copy_y0 必然偶数（裁剪时 max(0, -dst_x) ，-偶数 还是偶数）。 */
    (void)x0_dst;  /* 上行注释解释为何不需再对齐 */
    (void)y0_dst;

    /* 把 copy_w / copy_h 向下取整到偶数。 */
    copy_w &= ~1;
    copy_h &= ~1;
    if (copy_w <= 0 || copy_h <= 0) return true;

    /* ──────── 主循环：按 2×2 块迭代 ──────── */
    for (int by = 0; by < copy_h; by += 2) {
        const int sy0 = y0_src + by;
        const int sy1 = sy0 + 1;
        const int dy0 = dst_y + copy_y0 + by;
        const int dy1 = dy0 + 1;

        const uint8_t* src_row0 = src.data + sy0 * src.row_bytes;
        const uint8_t* src_row1 = src.data + sy1 * src.row_bytes;

        uint8_t* y_row0 = dst.y + dy0 * dst.y_stride;
        uint8_t* y_row1 = dst.y + dy1 * dst.y_stride;

        /* U/V plane 在 2×2 块内只占一个位置：(dy/2, dx/2)。 */
        const int duv_y = (dy0) >> 1;
        uint8_t* u_row  = dst.u + duv_y * dst.u_stride;
        uint8_t* v_row  = dst.v + duv_y * dst.v_stride;

        for (int bx = 0; bx < copy_w; bx += 2) {
            const int sx0 = x0_src + bx;
            const int sx1 = sx0 + 1;
            const int dx0 = dst_x + copy_x0 + bx;
            const int dx1 = dx0 + 1;
            const int duv_x = dx0 >> 1;

            /* 取 src 2×2 像素 RGBA。 */
            const uint8_t* p00 = src_row0 + sx0 * 4;
            const uint8_t* p01 = src_row0 + sx1 * 4;
            const uint8_t* p10 = src_row1 + sx0 * 4;
            const uint8_t* p11 = src_row1 + sx1 * 4;

            /* α=0 短路：4 个像素都全透明时，整个 2×2 块 dst 不动。
             * 这同时是：
             *   1) 语义保证 —— "完全透明" 必须等于 "无修改"，否则
             *      会因 YUV→RGB→YUV 往返舍入污染原 dst（实测
             *      (60,100,150) 这类自然色会漂 ±1-3）；
             *   2) 性能优化 —— PAG 贴片大量像素 α=0，跳过整块的
             *      6 次 yuv_to_rgb + 4 次 premul_over + 4 次 rgb_to_y
             *      + 1 次 rgb_to_u + 1 次 rgb_to_v。 */
            if ((p00[3] | p01[3] | p10[3] | p11[3]) == 0) {
                continue;
            }

            /* 取 dst Y 2×2 像素 + 共享 U/V。 */
            const uint8_t y00 = y_row0[dx0];
            const uint8_t y01 = y_row0[dx1];
            const uint8_t y10 = y_row1[dx0];
            const uint8_t y11 = y_row1[dx1];
            const uint8_t u_shared = u_row[duv_x];
            const uint8_t v_shared = v_row[duv_x];

            /* dst 2×2 像素分别反算 RGB（每个像素 Y 不同、UV 共享）。 */
            uint8_t r00, g00, b00; yuv_to_rgb_bt601(y00, u_shared, v_shared, r00, g00, b00);
            uint8_t r01, g01, b01; yuv_to_rgb_bt601(y01, u_shared, v_shared, r01, g01, b01);
            uint8_t r10, g10, b10; yuv_to_rgb_bt601(y10, u_shared, v_shared, r10, g10, b10);
            uint8_t r11, g11, b11; yuv_to_rgb_bt601(y11, u_shared, v_shared, r11, g11, b11);

            /* 4 像素分别做 premul-over blend。 */
            uint8_t or00, og00, ob00; premul_over(p00[0], p00[1], p00[2], p00[3], r00, g00, b00, or00, og00, ob00);
            uint8_t or01, og01, ob01; premul_over(p01[0], p01[1], p01[2], p01[3], r01, g01, b01, or01, og01, ob01);
            uint8_t or10, og10, ob10; premul_over(p10[0], p10[1], p10[2], p10[3], r10, g10, b10, or10, og10, ob10);
            uint8_t or11, og11, ob11; premul_over(p11[0], p11[1], p11[2], p11[3], r11, g11, b11, or11, og11, ob11);

            /* 写回 Y plane：逐像素独立。 */
            y_row0[dx0] = rgb_to_y_bt601(or00, og00, ob00);
            y_row0[dx1] = rgb_to_y_bt601(or01, og01, ob01);
            y_row1[dx0] = rgb_to_y_bt601(or10, og10, ob10);
            y_row1[dx1] = rgb_to_y_bt601(or11, og11, ob11);

            /* 写回 U/V plane：对 2×2 块的混合后 RGB 求平均，再算 U/V。 */
            const int avg_r = (or00 + or01 + or10 + or11 + 2) >> 2;
            const int avg_g = (og00 + og01 + og10 + og11 + 2) >> 2;
            const int avg_b = (ob00 + ob01 + ob10 + ob11 + 2) >> 2;
            u_row[duv_x] = rgb_to_u_bt601(static_cast<uint8_t>(avg_r),
                                          static_cast<uint8_t>(avg_g),
                                          static_cast<uint8_t>(avg_b));
            v_row[duv_x] = rgb_to_v_bt601(static_cast<uint8_t>(avg_r),
                                          static_cast<uint8_t>(avg_g),
                                          static_cast<uint8_t>(avg_b));
        }
    }

    return true;
}

}  // namespace pag_blend
