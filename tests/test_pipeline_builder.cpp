//
// test_pipeline_builder.cpp
//
// PipelineBuilder 静态纯函数单元测试。覆盖 caps 字符串构造和 fps 分数化。
// 不测 build() 本身（依赖真实 GStreamer + V4L2 设备，归集成测试）。
//

#include <gtest/gtest.h>

#include "pipeline_builder.h"
#include "v4l2_prober.h"

using v4l2_prober::Capability;

namespace {

Capability make_cap(const std::string& media,
                    const std::string& raw,
                    uint32_t w, uint32_t h) {
    Capability c;
    c.media_type = media;
    c.raw_format = raw;
    c.width = w;
    c.height = h;
    return c;
}

} // namespace

// =============================================================================
// fps_to_fraction
// =============================================================================
TEST(FpsToFraction, IntegerFps) {
    EXPECT_EQ(PipelineBuilder::fps_to_fraction(30.0),  "30/1");
    EXPECT_EQ(PipelineBuilder::fps_to_fraction(60.0),  "60/1");
    EXPECT_EQ(PipelineBuilder::fps_to_fraction(9.0),   "9/1");
    EXPECT_EQ(PipelineBuilder::fps_to_fraction(120.0), "120/1");
}

TEST(FpsToFraction, NtscFractionalFps) {
    EXPECT_EQ(PipelineBuilder::fps_to_fraction(29.97),  "30000/1001");
    EXPECT_EQ(PipelineBuilder::fps_to_fraction(59.94),  "60000/1001");
    EXPECT_EQ(PipelineBuilder::fps_to_fraction(23.976), "24000/1001");
}

TEST(FpsToFraction, OddFractionalFallback) {
    // V4L2 探到的 120.10fps（USB 摄像头常见）→ 落到 1000 倍数兜底
    EXPECT_EQ(PipelineBuilder::fps_to_fraction(120.10), "120100/1000");
}

TEST(FpsToFraction, ZeroOrNegativeReturnsDefault) {
    EXPECT_EQ(PipelineBuilder::fps_to_fraction(0.0),  "30/1");
    EXPECT_EQ(PipelineBuilder::fps_to_fraction(-1.0), "30/1");
}

// =============================================================================
// make_input_caps
// =============================================================================
TEST(MakeInputCaps, JpegHasNoFormatField) {
    auto cap = make_cap("image/jpeg", "", 1280, 720);
    EXPECT_EQ(PipelineBuilder::make_input_caps(cap, 30.0),
              "image/jpeg,width=1280,height=720,framerate=30/1");
}

TEST(MakeInputCaps, RawIncludesFormatField) {
    auto cap = make_cap("video/x-raw", "YUY2", 640, 480);
    EXPECT_EQ(PipelineBuilder::make_input_caps(cap, 30.0),
              "video/x-raw,format=YUY2,width=640,height=480,framerate=30/1");
}

TEST(MakeInputCaps, NV12Format) {
    auto cap = make_cap("video/x-raw", "NV12", 1920, 1080);
    EXPECT_EQ(PipelineBuilder::make_input_caps(cap, 60.0),
              "video/x-raw,format=NV12,width=1920,height=1080,framerate=60/1");
}

TEST(MakeInputCaps, FractionalFpsPropagated) {
    auto cap = make_cap("image/jpeg", "", 1280, 720);
    auto s   = PipelineBuilder::make_input_caps(cap, 29.97);
    EXPECT_NE(s.find("framerate=30000/1001"), std::string::npos)
        << "expected NTSC fraction in: " << s;
}

TEST(MakeInputCaps, RawWithoutFormatFalls) {
    // 防御性：raw_format 为空时不应输出 format=,（造成非法 caps）
    auto cap = make_cap("video/x-raw", "", 320, 240);
    auto s   = PipelineBuilder::make_input_caps(cap, 30.0);
    EXPECT_EQ(s.find(",format="), std::string::npos)
        << "raw_format 为空时不应有 format= 字段，actual: " << s;
}
