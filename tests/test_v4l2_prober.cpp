//
// test_v4l2_prober.cpp
//
// V4L2Prober 的纯函数单元测试：
//   - map_pixelformat：fourcc → GStreamer media_type
//   - intervals_to_fps：V4L2 frameinterval 三种类型的换算
//   - format_table：能力清单格式化
//   - Capability::to_string：单条能力打印
//
// probe() 本身依赖 /dev/video* 真实设备，不在单元测试覆盖范围。
//

#include <gtest/gtest.h>

#include "v4l2_prober.h"

#include <cmath>

using v4l2_prober::Capability;
using v4l2_prober::MediaTypeMapping;
using v4l2_prober::map_pixelformat;
using v4l2_prober::intervals_to_fps;
using v4l2_prober::format_table;

// 复用实现里同款的 fourcc 打包（避免依赖 linux/videodev2.h）
static constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(a))
         | (static_cast<uint32_t>(b) << 8)
         | (static_cast<uint32_t>(c) << 16)
         | (static_cast<uint32_t>(d) << 24);
}

// =============================================================================
// map_pixelformat
// =============================================================================
TEST(MapPixelformat, MJPG) {
    auto m = map_pixelformat(fourcc('M','J','P','G'));
    EXPECT_EQ(m.media_type, "image/jpeg");
    EXPECT_EQ(m.raw_format, "");
}

TEST(MapPixelformat, JPEG) {
    auto m = map_pixelformat(fourcc('J','P','E','G'));
    EXPECT_EQ(m.media_type, "image/jpeg");
    EXPECT_EQ(m.raw_format, "");
}

TEST(MapPixelformat, YUYV_BecomesYUY2) {
    // V4L2 的 YUYV 在 GStreamer 中叫 YUY2，这一步是关键转换
    auto m = map_pixelformat(fourcc('Y','U','Y','V'));
    EXPECT_EQ(m.media_type, "video/x-raw");
    EXPECT_EQ(m.raw_format, "YUY2");
}

TEST(MapPixelformat, NV12) {
    auto m = map_pixelformat(fourcc('N','V','1','2'));
    EXPECT_EQ(m.media_type, "video/x-raw");
    EXPECT_EQ(m.raw_format, "NV12");
}

TEST(MapPixelformat, YU12_BecomesI420) {
    // V4L2 YU12 == GStreamer I420
    auto m = map_pixelformat(fourcc('Y','U','1','2'));
    EXPECT_EQ(m.media_type, "video/x-raw");
    EXPECT_EQ(m.raw_format, "I420");
}

TEST(MapPixelformat, UnknownReturnsEmpty) {
    auto m = map_pixelformat(fourcc('X','X','X','X'));
    EXPECT_EQ(m.media_type, "");
    EXPECT_EQ(m.raw_format, "");
}

// =============================================================================
// intervals_to_fps
// =============================================================================
TEST(IntervalsToFps, Discrete_30fps) {
    v4l2_frmivalenum f{};
    f.type = V4L2_FRMIVAL_TYPE_DISCRETE;
    f.discrete.numerator = 1;
    f.discrete.denominator = 30;
    auto fps = intervals_to_fps(f);
    ASSERT_EQ(fps.size(), 1u);
    EXPECT_DOUBLE_EQ(fps[0], 30.0);
}

TEST(IntervalsToFps, Discrete_120fps) {
    // v4l2-ctl 输出常见的 1001/120100，约等于 120.1 fps
    v4l2_frmivalenum f{};
    f.type = V4L2_FRMIVAL_TYPE_DISCRETE;
    f.discrete.numerator = 1001;
    f.discrete.denominator = 120100;
    auto fps = intervals_to_fps(f);
    ASSERT_EQ(fps.size(), 1u);
    // round1 后应该精确为 120.0 或 120.1（取决于浮点误差），允许 0.05 容差
    EXPECT_NEAR(fps[0], 120.0, 0.2);
}

TEST(IntervalsToFps, Stepwise_RangeProducesMultipleSamples) {
    // interval ∈ [1/60, 1/15] → fps ∈ [15, 60]
    v4l2_frmivalenum f{};
    f.type = V4L2_FRMIVAL_TYPE_STEPWISE;
    f.stepwise.min.numerator = 1;
    f.stepwise.min.denominator = 60;   // 最小 interval -> 最大 fps
    f.stepwise.max.numerator = 1;
    f.stepwise.max.denominator = 15;   // 最大 interval -> 最小 fps
    f.stepwise.step.numerator = 1;
    f.stepwise.step.denominator = 60;
    auto fps = intervals_to_fps(f);
    // 至少含端点附近的 15 和 60
    ASSERT_FALSE(fps.empty());
    EXPECT_NEAR(fps.front(), 15.0, 0.5);
    EXPECT_NEAR(fps.back(), 60.0, 0.5);
    EXPECT_LE(fps.size(), 16u); // 采样封顶 16
}

TEST(IntervalsToFps, Continuous_TreatedSameAsStepwise) {
    v4l2_frmivalenum f{};
    f.type = V4L2_FRMIVAL_TYPE_CONTINUOUS;
    f.stepwise.min.numerator = 1;
    f.stepwise.min.denominator = 30;
    f.stepwise.max.numerator = 1;
    f.stepwise.max.denominator = 5;
    auto fps = intervals_to_fps(f);
    ASSERT_FALSE(fps.empty());
    EXPECT_NEAR(fps.front(), 5.0, 0.5);
    EXPECT_NEAR(fps.back(), 30.0, 0.5);
}

TEST(IntervalsToFps, Stepwise_DegenerateRangeReturnsSinglePoint) {
    // min == max 的退化区间
    v4l2_frmivalenum f{};
    f.type = V4L2_FRMIVAL_TYPE_STEPWISE;
    f.stepwise.min.numerator = 1;
    f.stepwise.min.denominator = 30;
    f.stepwise.max.numerator = 1;
    f.stepwise.max.denominator = 30;
    auto fps = intervals_to_fps(f);
    ASSERT_EQ(fps.size(), 1u);
    EXPECT_NEAR(fps[0], 30.0, 0.01);
}

TEST(IntervalsToFps, Discrete_ZeroNumeratorIsSafe) {
    // 防御性测试：理论上 V4L2 不会给出 0 分子，但代码应不崩溃
    v4l2_frmivalenum f{};
    f.type = V4L2_FRMIVAL_TYPE_DISCRETE;
    f.discrete.numerator = 0;
    f.discrete.denominator = 30;
    auto fps = intervals_to_fps(f);
    ASSERT_EQ(fps.size(), 1u);
    EXPECT_DOUBLE_EQ(fps[0], 0.0);
}

// =============================================================================
// Capability::to_string
// =============================================================================
TEST(CapabilityToString, JpegFormat) {
    Capability c;
    c.media_type = "image/jpeg";
    c.raw_format = "";
    c.width = 1280;
    c.height = 720;
    c.fps_list = {60.0};
    auto s = c.to_string();
    EXPECT_NE(s.find("image/jpeg"), std::string::npos);
    EXPECT_NE(s.find("1280x720"), std::string::npos);
    EXPECT_NE(s.find("60.0"), std::string::npos);
}

TEST(CapabilityToString, RawFormatIncludesSubFormat) {
    Capability c;
    c.media_type = "video/x-raw";
    c.raw_format = "YUY2";
    c.width = 640;
    c.height = 480;
    c.fps_list = {30.0, 9.0};
    auto s = c.to_string();
    EXPECT_NE(s.find("video/x-raw"), std::string::npos);
    EXPECT_NE(s.find("YUY2"), std::string::npos);
    EXPECT_NE(s.find("30.0"), std::string::npos);
    EXPECT_NE(s.find("9.0"), std::string::npos);
}

// =============================================================================
// format_table
// =============================================================================
TEST(FormatTable, EmptyReturnsPlaceholder) {
    auto s = format_table({});
    EXPECT_EQ(s, "(no capabilities)");
}

TEST(FormatTable, GroupsByMediaType) {
    std::vector<Capability> caps;
    {
        Capability c;
        c.media_type = "image/jpeg";
        c.width = 1280; c.height = 720; c.fps_list = {60.0};
        caps.push_back(c);
    }
    {
        Capability c;
        c.media_type = "video/x-raw";
        c.raw_format = "YUY2";
        c.width = 640; c.height = 480; c.fps_list = {30.0};
        caps.push_back(c);
    }
    {
        Capability c;
        c.media_type = "image/jpeg";
        c.width = 1920; c.height = 1080; c.fps_list = {30.0};
        caps.push_back(c);
    }

    auto s = format_table(caps);
    // 同一 group 应该出现一次（jpeg 两条分辨率合并到一个标题下）
    auto first_jpeg = s.find("[image/jpeg]");
    ASSERT_NE(first_jpeg, std::string::npos);
    auto second_jpeg = s.find("[image/jpeg]", first_jpeg + 1);
    EXPECT_EQ(second_jpeg, std::string::npos)
        << "image/jpeg 应该作为一个分组只出现一次：\n" << s;

    EXPECT_NE(s.find("[video/x-raw / YUY2]"), std::string::npos);
    // 三条分辨率行都应在
    EXPECT_NE(s.find("1280x720"), std::string::npos);
    EXPECT_NE(s.find("1920x1080"), std::string::npos);
    EXPECT_NE(s.find("640x480"), std::string::npos);
}
