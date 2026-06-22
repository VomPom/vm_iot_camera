//
// Created by vompom on 2026/06/18.
//
// @Description
//   pagfilter 元素的最小单元测试。
//
//   覆盖范围（Stage 4.3 收尾后）：
//     - 静态注册：pagfilter_register_static() 多次调用幂等；
//     - 元素工厂：gst_element_factory_make("pagfilter", ...) 能拿到实例；
//     - Pad 模板：sink/src 都有 ALWAYS pad，caps 包含 video/x-raw + I420；
//     - passthrough：实例化后 BaseTransform 默认 passthrough=TRUE，
//       且 pag-file 为空时即使经过 set_caps 也仍 passthrough；
//     - launch 串：videotestsrc ! pagfilter ! fakesink 能进 PAUSED 状态
//       （macOS 下也可跑通，不依赖摄像头）；
//     - Stage 4.3 属性 `pag-file`：读写、空值语义、stub 退化语义；
//     - pag_sdk stub 行为：单测构建强制 VM_IOT_ENABLE_LIBPAG=0
//       （见 tests/CMakeLists.txt），固定 stub 契约。
//
//   本测试**不依赖** libpag / EGL：单测进程跑在 VM_IOT_ENABLE_LIBPAG=0 下，
//   Engine::Make 永远返 nullptr，pag-file 非空会被静默降级回 passthrough。
//   真正的 PAG 渲染验证留给树莓派上的 gst-launch smoke 脚本。
//

#include <gtest/gtest.h>

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

#include <cstring>
#include <vector>

#include "gstpagfilter.h"
#include "pag_sdk.h"

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
    /* Stage 4.3 起：实例化后默认 pag-file 为空 → passthrough=TRUE，
     * 等价于 identity。只有 pag-file 非空且 Engine::Make 成功时才会切到
     * 非 passthrough；本进程跑在 VM_IOT_ENABLE_LIBPAG=0 下，Engine 永远
     * 造不出来，因此 passthrough 是稳定可断言的初始状态。 */
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
        if (st->static_caps.string &&
            std::string(st->static_caps.string).find("I420") != std::string::npos) {
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

/* ────────── Stage 4.3：pag-file 属性 / 退化语义 ──────────
 * 单测构建走 VM_IOT_ENABLE_LIBPAG=0，所以 Engine::Make 永远返 nullptr。
 * 这一组用例固定「pag-file 非空也必须退化为 passthrough」的契约，
 * 防止 4.4 热切换实现误把 stub 路径打通。 */

TEST_F(PagFilterTest, PagFilePropertyReadWrite) {
    /* GObject 属性可读可写：set 后立刻 get 应得到相同串；
     * 设空字符串与设 NULL 等价，get 都应拿到空字符串。 */
    GstElement* el = gst_element_factory_make("pagfilter", nullptr);
    ASSERT_NE(el, nullptr);

    gchar* v = nullptr;
    g_object_get(el, "pag-file", &v, nullptr);
    /* 默认空串而非 NULL（g_param_spec_string 默认 ""） */
    ASSERT_NE(v, nullptr);
    EXPECT_STREQ(v, "");
    g_free(v);

    g_object_set(el, "pag-file", "/tmp/whatever.pag", nullptr);
    g_object_get(el, "pag-file", &v, nullptr);
    EXPECT_STREQ(v, "/tmp/whatever.pag");
    g_free(v);

    /* 写空串：等价于清除。 */
    g_object_set(el, "pag-file", "", nullptr);
    g_object_get(el, "pag-file", &v, nullptr);
    EXPECT_STREQ(v, "");
    g_free(v);

    gst_object_unref(el);
}

TEST_F(PagFilterTest, EmptyPagFileKeepsPassthroughThroughCaps) {
    /* 用 appsrc 模拟一帧 I420 上去，pag-file 为空时整段管线维持 passthrough，
     * appsink 拿到的 buffer 在字节层面与喂入的完全一致。
     * 注意：在 passthrough 模式 BaseTransform 不会调用 set_caps 的渲染分支，
     * 但 caps 协商仍会发生——本用例验证整条 set_caps 路径在「无 pag-file」
     * 输入下的稳定性。 */
    constexpr int W = 64, H = 64;
    const size_t y_sz = static_cast<size_t>(W) * H;
    const size_t uv_sz = y_sz / 4;
    const size_t frame_sz = y_sz + uv_sz * 2;

    GError* err = nullptr;
    /* 用 gst_parse_launch 拼一条 appsrc → pagfilter → appsink 链路。 */
    gchar* pipeline_desc = g_strdup_printf(
        "appsrc name=src is-live=false format=time "
        "caps=video/x-raw,format=I420,width=%d,height=%d,framerate=30/1 "
        "! pagfilter "
        "! appsink name=sink emit-signals=false sync=false",
        W, H);
    GstElement* pipe = gst_parse_launch(pipeline_desc, &err);
    g_free(pipeline_desc);
    if (!pipe) {
        /* macOS 下偶发缺 appsrc 插件——跳过而不是 FAIL，与项目其它单测一致。 */
        GST_WARNING("skip: gst_parse_launch failed: %s",
                    err ? err->message : "(null)");
        if (err) g_error_free(err);
        return;
    }

    GstElement* src  = gst_bin_get_by_name(GST_BIN(pipe), "src");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
    ASSERT_NE(src,  nullptr);
    ASSERT_NE(sink, nullptr);

    /* 喂一帧 dummy I420（全 128 Y / 全 128 UV）。 */
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, frame_sz, nullptr);
    ASSERT_NE(buf, nullptr);
    {
        GstMapInfo mi;
        ASSERT_TRUE(gst_buffer_map(buf, &mi, GST_MAP_WRITE));
        std::memset(mi.data, 128, frame_sz);
        gst_buffer_unmap(buf, &mi);
    }
    GST_BUFFER_PTS(buf)      = 0;
    GST_BUFFER_DURATION(buf) = GST_SECOND / 30;

    EXPECT_NE(gst_element_set_state(pipe, GST_STATE_PLAYING),
              GST_STATE_CHANGE_FAILURE);

    GstFlowReturn fr = GST_FLOW_OK;
    g_signal_emit_by_name(src, "push-buffer", buf, &fr);
    gst_buffer_unref(buf);
    EXPECT_EQ(fr, GST_FLOW_OK);
    g_signal_emit_by_name(src, "end-of-stream", &fr);

    /* 拉一帧出来：pull-sample 是 appsink 的同步阻塞接口。 */
    GstSample* sample = nullptr;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (sample != nullptr) {
        GstBuffer* out = gst_sample_get_buffer(sample);
        ASSERT_NE(out, nullptr);
        GstMapInfo mi;
        ASSERT_TRUE(gst_buffer_map(out, &mi, GST_MAP_READ));
        /* passthrough 字节级断言：全 128。 */
        bool all_128 = true;
        for (size_t i = 0; i < mi.size; ++i) {
            if (mi.data[i] != 128) { all_128 = false; break; }
        }
        EXPECT_TRUE(all_128) << "pagfilter passthrough modified bytes";
        gst_buffer_unmap(out, &mi);
        gst_sample_unref(sample);
    }

    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_object_unref(pipe);
}

TEST_F(PagFilterTest, NonEmptyPagFileFallsBackToPassthroughUnderStub) {
    /* VM_IOT_ENABLE_LIBPAG=0 下，pag_sdk::Engine::Make 永远返 nullptr。
     * 即使设了非空 pag-file，pagfilter 在 set_caps 中也必须退化为 passthrough，
     * 整条管线仍能正常 PAUSED；这是 Stage 4.3 故意选择的"不报错只降级"语义。 */
    GError* err = nullptr;
    GstElement* pipe = gst_parse_launch(
        "videotestsrc num-buffers=2 is-live=false "
        "! video/x-raw,format=I420,width=320,height=240,framerate=30/1 "
        "! pagfilter pag-file=/nonexistent/path/foo.pag "
        "! fakesink sync=false",
        &err);
    if (!pipe) {
        FAIL() << "gst_parse_launch: " << (err ? err->message : "(null)");
        if (err) g_error_free(err);
        return;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipe, GST_STATE_PAUSED);
    EXPECT_NE(ret, GST_STATE_CHANGE_FAILURE);
    GstState state, pending;
    ret = gst_element_get_state(pipe, &state, &pending, 2 * GST_SECOND);
    EXPECT_NE(ret, GST_STATE_CHANGE_FAILURE);

    /* 进 PAUSED 后 caps 已协商，pagfilter 应已退化为 passthrough。
     * 用迭代器在 pipe 里找到 pagfilter 实例并验证。 */
    GstIterator* it = gst_bin_iterate_elements(GST_BIN(pipe));
    GValue v = G_VALUE_INIT;
    bool seen_pagfilter = false;
    while (gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
        GstElement* el = GST_ELEMENT(g_value_get_object(&v));
        GstElementFactory* f = gst_element_get_factory(el);
        if (f && std::string(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)))
                  == "pagfilter") {
            EXPECT_TRUE(gst_base_transform_is_passthrough(GST_BASE_TRANSFORM(el)));
            seen_pagfilter = true;
        }
        g_value_unset(&v);
    }
    gst_iterator_free(it);
    EXPECT_TRUE(seen_pagfilter);

    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(pipe);
}

/* Stage 3：pag_sdk 抽象层 stub 分支验证。
 * 单测构建强制 VM_IOT_ENABLE_LIBPAG=0（见 tests/CMakeLists.txt），
 * 所以这里期望：
 *   - is_enabled() 返回 false；
 *   - sdk_version() 返回固定 "libpag(disabled)"；
 *   - selftest_load() 不论传什么路径都返回 false 且不崩。
 * 真集成分支（ON）的验证留给手工 / 集成测试；本用例锁定 stub 行为契约，
 * 防止后续 refactor 把 stub 路径误打通。 */
TEST_F(PagFilterTest, PagSdkStubBehavior) {
    EXPECT_FALSE(pag_sdk::is_enabled());
    EXPECT_EQ(pag_sdk::sdk_version(), std::string("libpag(disabled)"));
    /* 三种入参都应平稳返回 false：空路径、明显不存在的路径、像合法的路径。 */
    EXPECT_FALSE(pag_sdk::selftest_load(""));
    EXPECT_FALSE(pag_sdk::selftest_load("/nonexistent/path/foo.pag"));
    EXPECT_FALSE(pag_sdk::selftest_load("pag/PAG_LOGO.pag"));
}

} // namespace
