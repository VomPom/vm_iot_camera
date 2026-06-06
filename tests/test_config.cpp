//
// Created by vompom on 2026/06/06.
//
// @Description
//   单元测试：使用 GoogleTest 验证 Config 各子结构的默认值是否符合预期。
//

#include <gtest/gtest.h>

#include "config.h"

TEST(ConfigDefaults, ServerConfig) {
    ServerConfig s;
    EXPECT_EQ(s.port, 8554);
    EXPECT_EQ(s.mount, "/live");
}

TEST(ConfigDefaults, CaptureConfig) {
    CaptureConfig c;
    EXPECT_EQ(c.device, "/dev/video0");
    EXPECT_EQ(c.width, 1280);
    EXPECT_EQ(c.height, 720);
    EXPECT_EQ(c.framerate, 30);
    EXPECT_EQ(c.pixfmt, "NV12");
}

TEST(ConfigDefaults, EncoderConfig) {
    EncoderConfig e;
    EXPECT_EQ(e.backend, "x264");
    EXPECT_EQ(e.bitrate_kbps, 4000);
    EXPECT_EQ(e.gop, 30);
    EXPECT_EQ(e.bframes, 0);
}

TEST(ConfigDefaults, LogConfig) {
    LogConfig l;
    EXPECT_EQ(l.level, "info");
}

TEST(ConfigDefaults, TopLevelAggregation) {
    Config cfg;
    EXPECT_EQ(cfg.server.port, 8554);
    EXPECT_EQ(cfg.capture.width, 1280);
    EXPECT_EQ(cfg.encoder.backend, "x264");
    EXPECT_EQ(cfg.log.level, "info");
}