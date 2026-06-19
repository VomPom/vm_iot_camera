//
// Created by vompom on 2026/06/18.
//
// @Description
//   pagfilter 元素的最小单元测试。
//
//   覆盖范围（Stage 2 起）：
//     - 静态注册：pagfilter_register_static() 多次调用幂等；
//     - 元素工厂：gst_element_factory_make("pagfilter", ...) 能拿到实例；
//     - Pad 模板：sink/src 都有 ALWAYS pad，caps 包含 video/x-raw + I420；
//     - passthrough：实例化后 BaseTransform 默认 passthrough=TRUE
//       （Stage 1 的核心承诺；Stage 2 仅在 invert=FALSE 时仍然成立）；
//     - launch 串：videotestsrc ! pagfilter ! fakesink 能进 PAUSED 状态
//       （macOS 下也可跑通，不依赖摄像头）；
//     - GObject 属性 invert：默认 FALSE；set TRUE 后 passthrough 变 FALSE，
//       set FALSE 后 passthrough 复位 TRUE；
//     - 像素反相：手工灌一帧 I420 (Y=0x10, U=0x20, V=0x30) 通过
//       appsrc → pagfilter(invert=true) → appsink，断言每个 plane
//       都被映射成 0xEF / 0xDF / 0xCF（255 - c）。
//

#include <gtest/gtest.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>

#include <cstring>
#include <vector>

#include "gstpagfilter.h"

namespace {

/* gst_init 只能初始化一次，多次调用是安全的（内部计数）。
 * 用 fixture 而不是 SetUpTestSuite 是为了让每个 case 都拿到干净进程级状态。 */
class PagFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!gst_is_initialized()) {
            gst_init(nullptr, nullptr);
        }
        ASSERT_TRUE(pagfilter_register_static());
    }
};

TEST_F(PagFilterTest, RegisterIsIdempotent) {
    /* 反复注册不应崩、不应改变 factory 数量（GStreamer 内部按 name 去重）。 */
    EXPECT_TRUE(pagfilter_register_static());
    EXPECT_TRUE(pagfilter_register_static());
    GstElementFactory* f = gst_element_factory_find("pagfilter");
    ASSERT_NE(f, nullptr);
    gst_object_unref(f);
}

TEST_F(PagFilterTest, FactoryProducesElement) {
    GstElement* el = gst_element_factory_make("pagfilter", "pag_test");
    ASSERT_NE(el, nullptr);
    EXPECT_TRUE(GST_IS_BASE_TRANSFORM(el));
    gst_object_unref(el);
}

TEST_F(PagFilterTest, DefaultsToPassthrough) {
    /* Stage 1 的核心承诺：实例化后立即 passthrough=TRUE，
     * 等价于 identity，下游 buffer 与上游同地址。 */
    GstElement* el = gst_element_factory_make("pagfilter", nullptr);
    ASSERT_NE(el, nullptr);
    GstBaseTransform* trans = GST_BASE_TRANSFORM(el);
    EXPECT_TRUE(gst_base_transform_is_passthrough(trans));
    gst_object_unref(el);
}

TEST_F(PagFilterTest, PadTemplatesAdvertiseI420) {
    GstElementFactory* f = gst_element_factory_find("pagfilter");
    ASSERT_NE(f, nullptr);

    /* 至少 2 个 pad 模板（sink + src），且都是 ALWAYS。
     * caps 字符串里必须含 "I420"，否则下游协商无法落到 cfg.capture.pixfmt 默认值。 */
    const GList* tmpls = gst_element_factory_get_static_pad_templates(f);
    int sink_cnt = 0, src_cnt = 0;
    bool i420_seen = false;
    for (const GList* l = tmpls; l; l = l->next) {
        auto* st = static_cast<GstStaticPadTemplate*>(l->data);
        if (st->direction == GST_PAD_SINK) ++sink_cnt;
        if (st->direction == GST_PAD_SRC)  ++src_cnt;
        EXPECT_EQ(st->presence, GST_PAD_ALWAYS);
        if (st->static_caps.string && std::string(st->static_caps.string).find("I420") != std::string::npos) {
            i420_seen = true;
        }
    }
    EXPECT_GE(sink_cnt, 1);
    EXPECT_GE(src_cnt,  1);
    EXPECT_TRUE(i420_seen);

    gst_object_unref(f);
}

TEST_F(PagFilterTest, LaunchPipelineReachesPaused) {
    /* 端到端最小验证：用 videotestsrc 顶替摄像头，强制 I420 caps，
     * 经过 pagfilter passthrough，进 fakesink。
     * 期望管线能进 PAUSED——这就证明 caps 协商通过、pad link 成功。
     * 跨平台安全：不依赖 /dev/video* 或硬件解码器。 */
    GError*      err = nullptr;
    GstElement*  pipe = gst_parse_launch(
        "videotestsrc num-buffers=2 is-live=false "
        "! video/x-raw,format=I420,width=320,height=240,framerate=30/1 "
        "! pagfilter "
        "! fakesink sync=false",
        &err);
    if (!pipe) {
        FAIL() << "gst_parse_launch: " << (err ? err->message : "(null)");
        if (err) g_error_free(err);
        return;
    }
    GstStateChangeReturn ret = gst_element_set_state(pipe, GST_STATE_PAUSED);
    /* SUCCESS / ASYNC 都算通过：preroll 一帧就够。 */
    EXPECT_NE(ret, GST_STATE_CHANGE_FAILURE);

    /* 等最多 2 秒拿到 ASYNC 完成或 ERROR。 */
    GstState state, pending;
    ret = gst_element_get_state(pipe, &state, &pending, 2 * GST_SECOND);
    EXPECT_NE(ret, GST_STATE_CHANGE_FAILURE);

    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(pipe);
}

/* Stage 2：invert 属性默认值与 setter 触发 passthrough 切换。
 * 用 element 单实例做白盒验证，不构建完整 pipeline。 */
TEST_F(PagFilterTest, InvertPropertyTogglesPassthrough) {
    GstElement* el = gst_element_factory_make("pagfilter", nullptr);
    ASSERT_NE(el, nullptr);
    GstBaseTransform* trans = GST_BASE_TRANSFORM(el);

    /* 默认值：invert=FALSE & passthrough=TRUE。 */
    gboolean v = TRUE;
    g_object_get(el, "invert", &v, nullptr);
    EXPECT_FALSE(v);
    EXPECT_TRUE(gst_base_transform_is_passthrough(trans));

    /* set invert=TRUE → passthrough 应被切到 FALSE。 */
    g_object_set(el, "invert", TRUE, nullptr);
    g_object_get(el, "invert", &v, nullptr);
    EXPECT_TRUE(v);
    EXPECT_FALSE(gst_base_transform_is_passthrough(trans));

    /* set invert=FALSE → passthrough 复位 TRUE，行为等价 Stage 1。 */
    g_object_set(el, "invert", FALSE, nullptr);
    g_object_get(el, "invert", &v, nullptr);
    EXPECT_FALSE(v);
    EXPECT_TRUE(gst_base_transform_is_passthrough(trans));

    gst_object_unref(el);
}

/* Stage 2：颜色反相像素级验证。
 *   构造一帧 4x4 I420 全 plane 同色 (Y=0x10, U=0x20, V=0x30)，
 *   走 appsrc → pagfilter(invert=true) → appsink，
 *   读出 buffer 后断言每 plane 已变成 0xEF / 0xDF / 0xCF。
 *   选 4x4 是为了让 U/V plane 至少 2x2 仍可验证 stride 正确。 */
TEST_F(PagFilterTest, InvertPipelineFlipsAllPlanes) {
    constexpr int kW = 4;
    constexpr int kH = 4;
    /* I420 大小 = w*h (Y) + 2 * (w/2)*(h/2) (U+V) = w*h*3/2 */
    constexpr gsize kSize = kW * kH * 3 / 2;

    GError* err = nullptr;
    GstElement* pipe = gst_parse_launch(
        "appsrc name=src is-live=false format=time block=true "
        "  caps=video/x-raw,format=I420,width=4,height=4,framerate=30/1 "
        "! pagfilter name=pag0 invert=true "
        "! appsink name=sink sync=false emit-signals=false",
        &err);
    ASSERT_NE(pipe, nullptr) << (err ? err->message : "(null)");
    if (err) g_error_free(err);

    GstElement* src  = gst_bin_get_by_name(GST_BIN(pipe), "src");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
    GstElement* pag  = gst_bin_get_by_name(GST_BIN(pipe), "pag0");
    ASSERT_NE(src,  nullptr);
    ASSERT_NE(sink, nullptr);
    ASSERT_NE(pag,  nullptr);

    /* invert=true 时 passthrough 应被切到 FALSE（防御性确认）。 */
    EXPECT_FALSE(gst_base_transform_is_passthrough(GST_BASE_TRANSFORM(pag)));

    ASSERT_NE(gst_element_set_state(pipe, GST_STATE_PLAYING),
              GST_STATE_CHANGE_FAILURE);

    /* 灌入一帧 (Y=0x10, U=0x20, V=0x30)。 */
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, kSize, nullptr);
    ASSERT_NE(buf, nullptr);
    {
        GstMapInfo m;
        ASSERT_TRUE(gst_buffer_map(buf, &m, GST_MAP_WRITE));
        std::memset(m.data,                          0x10, kW * kH);                    // Y
        std::memset(m.data + kW * kH,                0x20, (kW / 2) * (kH / 2));        // U
        std::memset(m.data + kW * kH + (kW / 2) * (kH / 2),
                    0x30, (kW / 2) * (kH / 2));                                          // V
        gst_buffer_unmap(buf, &m);
    }
    GST_BUFFER_PTS(buf) = 0;
    GST_BUFFER_DURATION(buf) = GST_SECOND / 30;

    GstFlowReturn pushed = gst_app_src_push_buffer(GST_APP_SRC(src), buf); // 拥有权转移
    ASSERT_EQ(pushed, GST_FLOW_OK);
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    /* 拉一帧，2 秒超时足够。 */
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                                     2 * GST_SECOND);
    ASSERT_NE(sample, nullptr) << "appsink 未拉到样本（pagfilter 没透出？）";
    GstBuffer* outbuf = gst_sample_get_buffer(sample);
    ASSERT_NE(outbuf, nullptr);

    GstMapInfo m;
    ASSERT_TRUE(gst_buffer_map(outbuf, &m, GST_MAP_READ));
    ASSERT_EQ(m.size, kSize);
    /* 期望：每 plane 全部被映射到 255 - c。 */
    for (gsize i = 0; i < static_cast<gsize>(kW * kH); ++i) {
        ASSERT_EQ(m.data[i], 0xEFu) << "Y plane @" << i;
    }
    for (gsize i = 0; i < static_cast<gsize>((kW / 2) * (kH / 2)); ++i) {
        ASSERT_EQ(m.data[kW * kH + i], 0xDFu) << "U plane @" << i;
    }
    for (gsize i = 0; i < static_cast<gsize>((kW / 2) * (kH / 2)); ++i) {
        ASSERT_EQ(m.data[kW * kH + (kW / 2) * (kH / 2) + i], 0xCFu)
            << "V plane @" << i;
    }
    gst_buffer_unmap(outbuf, &m);
    gst_sample_unref(sample);

    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_object_unref(pag);
    gst_object_unref(pipe);
}

} // namespace
