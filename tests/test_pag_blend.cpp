//
// Created by vompom on 2026/06/21.
//
// @Description
//   算法层单测：pag_blend::blend_rgba_premul_over_i420。
//
//   测试策略：
//     - 颜色空间 helper（rgb_to_y/u/v）先单独验证，再验证 blend；
//     - 退化场景：α=0 / α=255 给出严格期望值（容差 ±2，因为 BT.601 limited
//       的 RGB↔YUV 反复转换有舍入损失；退化场景的 ±2 是经验值）；
//     - 任意 α：只验证「带 PAG > 不带 PAG 时的方向正确」（unblended 与 src
//       色彩在 Y 通道上单调可比），避免单测过早锁死实现细节；
//     - stride 大于 width 的情形单独覆盖，因为这是 GStreamer 实际给的形态；
//     - 边界裁剪：负偏移 / 超右下角；
//     - 非法输入：奇数 dst_x / 奇数 dst.width / row_bytes 不足，应返回 false
//       且 dst 完全不被修改。
//

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "pag_blend.h"

namespace {

/* I420 缓冲管理小工具：用单一连续 vector 模拟 plane stride > width 的情形。 */
struct I420Buf {
    std::vector<uint8_t> bytes;
    int width;
    int height;
    int y_stride;
    int u_stride;
    int v_stride;
    size_t y_off, u_off, v_off;

    static I420Buf Make(int w, int h, uint8_t init_y, uint8_t init_u, uint8_t init_v,
                        int extra_y_stride = 0,
                        int extra_uv_stride = 0) {
        I420Buf buf;
        buf.width    = w;
        buf.height   = h;
        buf.y_stride = w + extra_y_stride;
        buf.u_stride = w / 2 + extra_uv_stride;
        buf.v_stride = w / 2 + extra_uv_stride;

        buf.y_off = 0;
        buf.u_off = buf.y_off + static_cast<size_t>(buf.y_stride) * h;
        buf.v_off = buf.u_off + static_cast<size_t>(buf.u_stride) * (h / 2);
        size_t total = buf.v_off + static_cast<size_t>(buf.v_stride) * (h / 2);
        buf.bytes.assign(total, 0);

        /* 填充初值 */
        for (int y = 0; y < h; ++y)
            std::memset(buf.bytes.data() + buf.y_off + y * buf.y_stride, init_y, w);
        for (int y = 0; y < h / 2; ++y) {
            std::memset(buf.bytes.data() + buf.u_off + y * buf.u_stride, init_u, w / 2);
            std::memset(buf.bytes.data() + buf.v_off + y * buf.v_stride, init_v, w / 2);
        }
        return buf;
    }

    pag_blend::I420Frame view() {
        pag_blend::I420Frame f;
        f.y = bytes.data() + y_off;
        f.u = bytes.data() + u_off;
        f.v = bytes.data() + v_off;
        f.width    = width;
        f.height   = height;
        f.y_stride = y_stride;
        f.u_stride = u_stride;
        f.v_stride = v_stride;
        return f;
    }
};

/* RGBA premul 缓冲构造：把 unpremul 的 RGBA 先按 a 预乘后写入。 */
std::vector<uint8_t> MakeRgbaPremul(int w, int h,
                                    uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                    int extra_row_bytes = 0) {
    const int row_bytes = w * 4 + extra_row_bytes;
    std::vector<uint8_t> buf(static_cast<size_t>(row_bytes) * h, 0);
    const uint8_t pr = static_cast<uint8_t>((static_cast<int>(r) * a + 127) / 255);
    const uint8_t pg = static_cast<uint8_t>((static_cast<int>(g) * a + 127) / 255);
    const uint8_t pb = static_cast<uint8_t>((static_cast<int>(b) * a + 127) / 255);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = buf.data() + y * row_bytes;
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = pr;
            row[x * 4 + 1] = pg;
            row[x * 4 + 2] = pb;
            row[x * 4 + 3] = a;
        }
    }
    return buf;
}

inline int absdiff(int a, int b) { return std::abs(a - b); }

}  // namespace

/* ─────────────────────── 颜色空间 helper ─────────────────────── */

TEST(PagBlendColor, Bt601LimitedKnownColors) {
    /* 纯黑 (0,0,0) → Y=16, U=V=128（BT.601 limited range 黑电平）。 */
    EXPECT_EQ(pag_blend::rgb_to_y_bt601(0, 0, 0), 16);
    EXPECT_EQ(pag_blend::rgb_to_u_bt601(0, 0, 0), 128);
    EXPECT_EQ(pag_blend::rgb_to_v_bt601(0, 0, 0), 128);

    /* 纯白 (255,255,255) → Y≈235, U=V≈128（容差 ±1 由 8-bit 定点引入）。 */
    EXPECT_NEAR(pag_blend::rgb_to_y_bt601(255, 255, 255), 235, 1);
    EXPECT_NEAR(pag_blend::rgb_to_u_bt601(255, 255, 255), 128, 1);
    EXPECT_NEAR(pag_blend::rgb_to_v_bt601(255, 255, 255), 128, 1);

    /* 纯红 (255,0,0) → V 应明显高于 128（红色色度信号正方向）。 */
    EXPECT_GT(pag_blend::rgb_to_v_bt601(255, 0, 0), 200);
    EXPECT_LT(pag_blend::rgb_to_u_bt601(255, 0, 0), 100);

    /* 纯蓝 (0,0,255) → U 应明显高于 128（蓝色色度信号正方向）。 */
    EXPECT_GT(pag_blend::rgb_to_u_bt601(0, 0, 255), 200);
    EXPECT_LT(pag_blend::rgb_to_v_bt601(0, 0, 255), 100);
}

/* ─────────────────────── 完全透明：dst 完全不变 ─────────────────────── */

TEST(PagBlend, FullyTransparentLeavesDstUnchanged) {
    /* 4×4 灰色底图，I420 (Y=128, U=128, V=128)。 */
    I420Buf dst = I420Buf::Make(4, 4, 128, 128, 128);
    I420Buf dst_orig = dst;  // snapshot

    /* 4×4 RGBA premul，alpha=0 → 所有 RGB 也是 0（premul 后）。 */
    auto src_bytes = MakeRgbaPremul(4, 4, 200, 100, 50, 0);
    pag_blend::RgbaPremulFrame src{src_bytes.data(), 4, 4, 4 * 4};

    ASSERT_TRUE(pag_blend::blend_rgba_premul_over_i420(src, dst.view(), 0, 0));

    EXPECT_EQ(dst.bytes, dst_orig.bytes);
}

TEST(PagBlend, FullyTransparentLeavesNaturalColorDstUnchanged) {
    /* 真实摄像头帧的"自然色"底图：YUV=(60,100,150)（暖肤色范围之一）。
     * 这类色彩在 YUV→RGB→YUV 往返时会漂 ±1-3，因此 α=0 短路是 must-have。
     * 用例对全 4×4 dst 字节严格比对。 */
    I420Buf dst = I420Buf::Make(4, 4, 60, 100, 150);
    I420Buf dst_orig = dst;

    auto src_bytes = MakeRgbaPremul(4, 4, 200, 100, 50, 0);
    pag_blend::RgbaPremulFrame src{src_bytes.data(), 4, 4, 4 * 4};

    ASSERT_TRUE(pag_blend::blend_rgba_premul_over_i420(src, dst.view(), 0, 0));
    EXPECT_EQ(dst.bytes, dst_orig.bytes);
}

/* ─────────────────────── 完全不透明：dst 被 src 完全覆盖 ─────────────────────── */

TEST(PagBlend, FullyOpaqueReplacesDst) {
    /* 4×4 灰色底图。 */
    I420Buf dst = I420Buf::Make(4, 4, 128, 128, 128);

    /* 4×4 纯红 RGBA premul，alpha=255。 */
    const uint8_t R = 255, G = 0, B = 0, A = 255;
    auto src_bytes = MakeRgbaPremul(4, 4, R, G, B, A);
    pag_blend::RgbaPremulFrame src{src_bytes.data(), 4, 4, 4 * 4};

    ASSERT_TRUE(pag_blend::blend_rgba_premul_over_i420(src, dst.view(), 0, 0));

    const uint8_t expected_y = pag_blend::rgb_to_y_bt601(R, G, B);
    const uint8_t expected_u = pag_blend::rgb_to_u_bt601(R, G, B);
    const uint8_t expected_v = pag_blend::rgb_to_v_bt601(R, G, B);

    /* Y 平面：每个像素都应该 ≈ expected_y。容差 ±2 是因为 BT.601 RGB↔YUV
     * 8-bit 整数定点的反复转换会有舍入损失（dst 像素先 YUV→RGB 再 RGB→YUV）。
     * α=255 时这个反复转换链路其实应该被 premul_over 短路（src_rgb +
     * dst_rgb × 0 = src_rgb），所以容差极小：理想下 ±1，BT.601 定点系数舍入
     * 可能再 +1。 */
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t v = dst.bytes[dst.y_off + y * dst.y_stride + x];
            EXPECT_NEAR(v, expected_y, 2)
                << "Y(" << x << "," << y << ")=" << static_cast<int>(v);
        }
    }
    /* U/V 平面：2×2 块共一格，对纯色 src 也应 ≈ expected_u/v。 */
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            uint8_t u = dst.bytes[dst.u_off + y * dst.u_stride + x];
            uint8_t vv = dst.bytes[dst.v_off + y * dst.v_stride + x];
            EXPECT_NEAR(u,  expected_u, 2);
            EXPECT_NEAR(vv, expected_v, 2);
        }
    }
}

/* ─────────────────────── 半透明：方向正确 ─────────────────────── */

TEST(PagBlend, SemiTransparentMovesTowardSrc) {
    /* 4×4 灰色底图 (Y=128, U=128, V=128)。 */
    I420Buf dst = I420Buf::Make(4, 4, 128, 128, 128);

    /* 4×4 纯白 RGBA premul，alpha=128（半透明）。 */
    auto src_bytes = MakeRgbaPremul(4, 4, 255, 255, 255, 128);
    pag_blend::RgbaPremulFrame src{src_bytes.data(), 4, 4, 4 * 4};

    ASSERT_TRUE(pag_blend::blend_rgba_premul_over_i420(src, dst.view(), 0, 0));

    /* 期望：Y 介于灰底 128 与纯白 Y(=235) 之间，靠近中点偏白侧。
     * 容差给 [150, 220]：宽，但能确保「往白色移动」方向是对的。 */
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t v = dst.bytes[dst.y_off + y * dst.y_stride + x];
            EXPECT_GT(v, 150) << "Y(" << x << "," << y << ")";
            EXPECT_LT(v, 220) << "Y(" << x << "," << y << ")";
        }
    }
}

/* ─────────────────────── stride > width ─────────────────────── */

TEST(PagBlend, NonTightStridesRespected) {
    /* 给 Y plane 加 +7 字节 stride，U/V plane 加 +3 字节 stride。 */
    I420Buf dst = I420Buf::Make(8, 4, 50, 100, 200,
                                /*extra_y_stride=*/7,
                                /*extra_uv_stride=*/3);
    /* 在每个 stride 末尾的 padding 区域写入哨兵 0xAB，blend 后这些字节
     * 必须保持不变（否则说明实现把 stride 当成 width 用了）。 */
    for (int y = 0; y < 4; ++y) {
        uint8_t* row = dst.bytes.data() + dst.y_off + y * dst.y_stride;
        for (int x = dst.width; x < dst.y_stride; ++x) row[x] = 0xAB;
    }
    for (int y = 0; y < 2; ++y) {
        uint8_t* u = dst.bytes.data() + dst.u_off + y * dst.u_stride;
        uint8_t* v = dst.bytes.data() + dst.v_off + y * dst.v_stride;
        for (int x = dst.width / 2; x < dst.u_stride; ++x) u[x] = 0xAB;
        for (int x = dst.width / 2; x < dst.v_stride; ++x) v[x] = 0xAB;
    }

    /* 不透明纯红 4×4，贴到 (0,0)。覆盖 dst 左上 4×4，其余 4×4 区域不动。 */
    auto src_bytes = MakeRgbaPremul(4, 4, 255, 0, 0, 255);
    pag_blend::RgbaPremulFrame src{src_bytes.data(), 4, 4, 4 * 4};
    ASSERT_TRUE(pag_blend::blend_rgba_premul_over_i420(src, dst.view(), 0, 0));

    /* 哨兵检查：padding 字节必须仍是 0xAB。 */
    for (int y = 0; y < 4; ++y) {
        const uint8_t* row = dst.bytes.data() + dst.y_off + y * dst.y_stride;
        for (int x = dst.width; x < dst.y_stride; ++x) {
            EXPECT_EQ(row[x], 0xAB) << "Y stride padding @ (" << x << "," << y << ")";
        }
    }
    for (int y = 0; y < 2; ++y) {
        const uint8_t* u = dst.bytes.data() + dst.u_off + y * dst.u_stride;
        const uint8_t* v = dst.bytes.data() + dst.v_off + y * dst.v_stride;
        for (int x = dst.width / 2; x < dst.u_stride; ++x) {
            EXPECT_EQ(u[x], 0xAB) << "U stride padding @ (" << x << "," << y << ")";
            EXPECT_EQ(v[x], 0xAB) << "V stride padding @ (" << x << "," << y << ")";
        }
    }

    /* 右侧未被覆盖的 4×4 区域 Y 应仍是 50（初始灰底）。 */
    for (int y = 0; y < 4; ++y) {
        for (int x = 4; x < 8; ++x) {
            uint8_t v = dst.bytes[dst.y_off + y * dst.y_stride + x];
            EXPECT_EQ(v, 50) << "untouched Y @ (" << x << "," << y << ")";
        }
    }
}

/* ─────────────────────── 裁剪：负偏移 / 超右下 ─────────────────────── */

TEST(PagBlend, ClipsAgainstDstBounds) {
    I420Buf dst = I420Buf::Make(8, 8, 100, 100, 100);
    I420Buf dst_orig = dst;

    /* 4×4 不透明纯红，贴到 (-10,-10)：完全在 dst 外，no-op。 */
    auto src = MakeRgbaPremul(4, 4, 255, 0, 0, 255);
    pag_blend::RgbaPremulFrame s{src.data(), 4, 4, 4 * 4};

    EXPECT_TRUE(pag_blend::blend_rgba_premul_over_i420(s, dst.view(), -10, -10));
    EXPECT_EQ(dst.bytes, dst_orig.bytes);

    /* 贴到 (10,10)：完全超出 dst 右下，no-op。 */
    EXPECT_TRUE(pag_blend::blend_rgba_premul_over_i420(s, dst.view(), 10, 10));
    EXPECT_EQ(dst.bytes, dst_orig.bytes);

    /* 贴到 (6,6)：只有 src 左上 2×2 落入 dst 右下 2×2。
     * dst 左上 6×6 区域应完全不变。 */
    EXPECT_TRUE(pag_blend::blend_rgba_premul_over_i420(s, dst.view(), 6, 6));
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 6; ++x) {
            uint8_t v = dst.bytes[dst.y_off + y * dst.y_stride + x];
            EXPECT_EQ(v, 100) << "untouched dst @ (" << x << "," << y << ")";
        }
    }
    /* 右下 2×2 应被红色覆盖（Y 远小于 100）。 */
    const uint8_t red_y = pag_blend::rgb_to_y_bt601(255, 0, 0);
    for (int y = 6; y < 8; ++y) {
        for (int x = 6; x < 8; ++x) {
            uint8_t v = dst.bytes[dst.y_off + y * dst.y_stride + x];
            EXPECT_NEAR(v, red_y, 2);
        }
    }
}

/* ─────────────────────── 非法输入：完全不修改 dst ─────────────────────── */

TEST(PagBlend, RejectsInvalidInputs) {
    I420Buf dst = I420Buf::Make(8, 8, 77, 88, 99);
    I420Buf dst_orig = dst;

    auto src_bytes = MakeRgbaPremul(4, 4, 255, 0, 0, 255);
    pag_blend::RgbaPremulFrame src{src_bytes.data(), 4, 4, 4 * 4};

    /* dst_x 奇数。 */
    EXPECT_FALSE(pag_blend::blend_rgba_premul_over_i420(src, dst.view(), 1, 0));
    EXPECT_EQ(dst.bytes, dst_orig.bytes);

    /* dst_y 奇数。 */
    EXPECT_FALSE(pag_blend::blend_rgba_premul_over_i420(src, dst.view(), 0, 3));
    EXPECT_EQ(dst.bytes, dst_orig.bytes);

    /* src 指针为空。 */
    pag_blend::RgbaPremulFrame null_src{nullptr, 4, 4, 4 * 4};
    EXPECT_FALSE(pag_blend::blend_rgba_premul_over_i420(null_src, dst.view(), 0, 0));
    EXPECT_EQ(dst.bytes, dst_orig.bytes);

    /* row_bytes 不足。 */
    pag_blend::RgbaPremulFrame thin{src_bytes.data(), 4, 4, 4 * 4 - 1};
    EXPECT_FALSE(pag_blend::blend_rgba_premul_over_i420(thin, dst.view(), 0, 0));
    EXPECT_EQ(dst.bytes, dst_orig.bytes);

    /* dst y_stride 不足。 */
    pag_blend::I420Frame bad_dst = dst.view();
    bad_dst.y_stride = dst.width - 1;
    EXPECT_FALSE(pag_blend::blend_rgba_premul_over_i420(src, bad_dst, 0, 0));
    EXPECT_EQ(dst.bytes, dst_orig.bytes);

    /* dst.width 奇数。 */
    pag_blend::I420Frame odd_w = dst.view();
    odd_w.width = 7;
    EXPECT_FALSE(pag_blend::blend_rgba_premul_over_i420(src, odd_w, 0, 0));
    EXPECT_EQ(dst.bytes, dst_orig.bytes);
}

/* ─────────────────────── 0 像素 src：no-op 但成功 ─────────────────────── */

TEST(PagBlend, ZeroSizedSrcIsNoOp) {
    I420Buf dst = I420Buf::Make(4, 4, 200, 200, 200);
    I420Buf dst_orig = dst;

    auto src_bytes = MakeRgbaPremul(4, 4, 255, 0, 0, 255);  /* 内容无关 */
    pag_blend::RgbaPremulFrame zero_w{src_bytes.data(), 0, 4, 4};
    pag_blend::RgbaPremulFrame zero_h{src_bytes.data(), 4, 0, 4 * 4};

    EXPECT_TRUE(pag_blend::blend_rgba_premul_over_i420(zero_w, dst.view(), 0, 0));
    EXPECT_TRUE(pag_blend::blend_rgba_premul_over_i420(zero_h, dst.view(), 0, 0));
    EXPECT_EQ(dst.bytes, dst_orig.bytes);
}

/* ─────────── 端到端：左半 α=0 右半 α=255，验证两种 fast-path 并存 ─────────── */
TEST(PagBlend, MixedAlphaRegionsAreCorrect) {
    /* 8×4 自然色底图 YUV=(60,100,150)。 */
    I420Buf dst = I420Buf::Make(8, 4, 60, 100, 150);
    I420Buf dst_orig = dst;

    /* 8×4 src：左半 4 列 alpha=0（pre-mul 后全 0），右半 4 列纯红 alpha=255。 */
    const int row_bytes = 8 * 4;
    std::vector<uint8_t> src(static_cast<size_t>(row_bytes) * 4, 0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 4; x < 8; ++x) {
            uint8_t* px = src.data() + y * row_bytes + x * 4;
            px[0] = 255; px[1] = 0; px[2] = 0; px[3] = 255;
        }
    }
    pag_blend::RgbaPremulFrame s{src.data(), 8, 4, row_bytes};
    ASSERT_TRUE(pag_blend::blend_rgba_premul_over_i420(s, dst.view(), 0, 0));

    /* 左 4 列（含其 U/V 即 4/2=2 列）：dst 完全不变。 */
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const auto* expected = dst_orig.bytes.data() + dst_orig.y_off + y * dst_orig.y_stride;
            EXPECT_EQ(dst.bytes[dst.y_off + y * dst.y_stride + x], expected[x])
                << "left Y @ (" << x << "," << y << ")";
        }
    }
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const auto* expected_u = dst_orig.bytes.data() + dst_orig.u_off + y * dst_orig.u_stride;
            const auto* expected_v = dst_orig.bytes.data() + dst_orig.v_off + y * dst_orig.v_stride;
            EXPECT_EQ(dst.bytes[dst.u_off + y * dst.u_stride + x], expected_u[x])
                << "left U @ (" << x << "," << y << ")";
            EXPECT_EQ(dst.bytes[dst.v_off + y * dst.v_stride + x], expected_v[x])
                << "left V @ (" << x << "," << y << ")";
        }
    }

    /* 右 4 列：纯红 α=255，Y 应接近 BT.601 红 Y(=82)，容差 ±2。 */
    const uint8_t red_y = pag_blend::rgb_to_y_bt601(255, 0, 0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 4; x < 8; ++x) {
            uint8_t v = dst.bytes[dst.y_off + y * dst.y_stride + x];
            EXPECT_NEAR(v, red_y, 2) << "right Y @ (" << x << "," << y << ")";
        }
    }
}
