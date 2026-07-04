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

// =============================================================================
// build() — face 副线开关
//
// 注：PipelineBuilder::build 内部会 v4l2_prober::probe(c.capture.device)。
// 在 CI / macOS 上设备不存在时会走兜底"按配置硬拼"路径，launch 串照样有效。
// 所有 face 用例都断言 launch 串里"包含 / 不包含"特定 token，与设备无关。
// =============================================================================

namespace {

Config make_baseline_cfg() {
    Config c;
    /* 默认就够用：face.enabled=false, filter.enabled=true, encoder=x264。
     * 关掉 GL 滤镜让 launch 串更短，断言更稳定。 */
    c.filter.enabled = false;
    c.filter.pag.enabled = false;
    return c;
}

} // namespace

TEST(BuildFace, DisabledByDefault) {
    auto c = make_baseline_cfg();
    ASSERT_FALSE(c.face.enabled);
    auto s = PipelineBuilder::build(c);

    EXPECT_EQ(s.find("facedetect"),       std::string::npos) << s;
    EXPECT_EQ(s.find("face_valve"),       std::string::npos) << s;
    EXPECT_EQ(s.find("face_appsink"),     std::string::npos) << s;
    EXPECT_EQ(s.find("face_prev_valve"),  std::string::npos) << s;
    EXPECT_EQ(s.find("face_jpeg_sink"),   std::string::npos) << s;
}

TEST(BuildFace, EnabledMainBranch) {
    auto c = make_baseline_cfg();
    c.face.enabled                  = true;
    c.face.detect.cascade           = "/usr/share/opencv4/haarcascades/haarcascade_frontalface_default.xml";
    c.face.detect.min_size_px       = 80;
    c.face.rate.fps_limit           = 5;
    c.face.preview_jpeg.enabled     = false;
    c.face.control.enabled_at_start = true;

    auto s = PipelineBuilder::build(c);

    EXPECT_NE(s.find("facedetect name=face0"), std::string::npos) << s;
    EXPECT_NE(s.find("appsink name=face_appsink"), std::string::npos) << s;
    EXPECT_NE(s.find("valve name=face_valve drop=false"), std::string::npos) << s;
    EXPECT_NE(s.find("framerate=5/1"), std::string::npos) << s;
    EXPECT_NE(s.find("min-size-width=80"),  std::string::npos) << s;
    EXPECT_NE(s.find("min-size-height=80"), std::string::npos) << s;
    EXPECT_NE(s.find("video/x-raw,format=RGB"), std::string::npos) << s;

    /* 主检测路径 display=false；preview 路径 display=true，本用例未启用，应不出现。 */
    EXPECT_NE(s.find("display=false"), std::string::npos) << s;
    EXPECT_EQ(s.find("display=true"),  std::string::npos) << s;
    EXPECT_EQ(s.find("face_prev_valve"), std::string::npos) << s;
    EXPECT_EQ(s.find("face_jpeg_sink"),  std::string::npos) << s;
}

TEST(BuildFace, EnabledAtStartFalseSetsDropTrue) {
    auto c = make_baseline_cfg();
    c.face.enabled                  = true;
    c.face.control.enabled_at_start = false;

    auto s = PipelineBuilder::build(c);
    EXPECT_NE(s.find("valve name=face_valve drop=true"), std::string::npos) << s;
}

TEST(BuildFace, FpsLimitZeroNoVideorate) {
    auto c = make_baseline_cfg();
    c.face.enabled        = true;
    c.face.rate.fps_limit = 0;       // 不限速
    auto s = PipelineBuilder::build(c);
    /* fps_limit=0 时 face 副线不应注入 framerate=N/1 caps；
     * 注意主线编码段不会出现"framerate="字面（主线 caps 用 framerate=<int>/1
     * 也会写出来，所以这里改用 face 段的 RGB caps 后紧跟 facedetect 验证）。 */
    EXPECT_EQ(s.find("video/x-raw,framerate="), std::string::npos) << s;
}

TEST(BuildFace, PreviewBranchAttached) {
    auto c = make_baseline_cfg();
    c.face.enabled                = true;
    c.face.preview_jpeg.enabled   = true;
    c.face.preview_jpeg.jpeg_quality = 60;
    c.face.preview_jpeg.fps_limit    = 2;

    auto s = PipelineBuilder::build(c);

    /* 主检测路径仍存在。 */
    EXPECT_NE(s.find("facedetect name=face0"),     std::string::npos) << s;
    /* preview 副线四件特征：display=true / jpegenc / face_prev_valve / face_jpeg_sink。 */
    EXPECT_NE(s.find("display=true"),              std::string::npos) << s;
    EXPECT_NE(s.find("jpegenc quality=60"),        std::string::npos) << s;
    EXPECT_NE(s.find("valve name=face_prev_valve"),std::string::npos) << s;
    EXPECT_NE(s.find("appsink name=face_jpeg_sink"),std::string::npos) << s;
    EXPECT_NE(s.find("framerate=2/1"),             std::string::npos) << s;
}

TEST(BuildFace, OptionalCascadesOmittedWhenEmpty) {
    auto c = make_baseline_cfg();
    c.face.enabled = true;
    /* profile/nose/mouth/eyes 全默认空 → launch 串中不应出现对应 *-location 字段。 */
    auto s = PipelineBuilder::build(c);

    EXPECT_EQ(s.find("profile-location="), std::string::npos) << s;
    EXPECT_EQ(s.find("nose-location="),    std::string::npos) << s;
    EXPECT_EQ(s.find("mouth-location="),   std::string::npos) << s;
    EXPECT_EQ(s.find("eyes-location="),    std::string::npos) << s;
}

TEST(BuildFace, OptionalCascadesIncludedWhenSet) {
    auto c = make_baseline_cfg();
    c.face.enabled        = true;
    c.face.detect.profile = "/tmp/profile.xml";
    c.face.detect.eyes    = "/tmp/eyes.xml";

    auto s = PipelineBuilder::build(c);
    EXPECT_NE(s.find("profile-location=\"/tmp/profile.xml\""), std::string::npos) << s;
    EXPECT_NE(s.find("eyes-location=\"/tmp/eyes.xml\""),       std::string::npos) << s;
    EXPECT_EQ(s.find("nose-location="),  std::string::npos) << s;
    EXPECT_EQ(s.find("mouth-location="), std::string::npos) << s;
}

TEST(BuildFace, MinSizePropagated) {
    auto c = make_baseline_cfg();
    c.face.enabled            = true;
    c.face.detect.min_size_px = 200;
    auto s = PipelineBuilder::build(c);
    EXPECT_NE(s.find("min-size-width=200"),  std::string::npos) << s;
    EXPECT_NE(s.find("min-size-height=200"), std::string::npos) << s;
}
