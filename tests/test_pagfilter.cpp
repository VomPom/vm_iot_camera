//
// Created by vompom on 2026/06/18.
//
// @Description
//   pagfilter 元素的最小单元测试。
//
//   覆盖范围（Stage 3 收尾后）：
//     - 静态注册：pagfilter_register_static() 多次调用幂等；
//     - 元素工厂：gst_element_factory_make("pagfilter", ...) 能拿到实例；
//     - Pad 模板：sink/src 都有 ALWAYS pad，caps 包含 video/x-raw + I420；
//     - passthrough：实例化后 BaseTransform 默认 passthrough=TRUE
//       （Stage 1 骨架的核心承诺，Stage 4 接入 PAG 渲染时才会切换）；
//     - launch 串：videotestsrc ! pagfilter ! fakesink 能进 PAUSED 状态
//       （macOS 下也可跑通，不依赖摄像头）；
//     - pag_sdk stub 行为：单测构建强制 VM_IOT_ENABLE_LIBPAG=0
//       （见 tests/CMakeLists.txt），固定 stub 契约。
//
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
    /* Stage 1 骨架的核心承诺：实例化后立即 passthrough=TRUE，
     * 等价于 identity，下游 buffer 与上游同地址。
     * Stage 4 接入 PAG 渲染时此承诺会被打破，本用例届时需要同步调整。 */
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
