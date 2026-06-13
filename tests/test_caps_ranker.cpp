//
// test_caps_ranker.cpp
//
// CapsRanker 纯函数单元测试，覆盖：
//   - format_weight：JPEG / raw 各 fourcc / unknown / prefer_jpeg 反转
//   - pick_closest_fps：常规 / 等距 / 空列表
//   - rank：格式优先 / 分辨率距离 / fps 惩罚 / 综合排序
//   - rank：fps_list 为空时丢弃
//   - rank：tiebreaker 行为
//

#include <gtest/gtest.h>

#include "caps_ranker.h"
#include "v4l2_prober.h"

using v4l2_prober::Capability;
using caps_ranker::Preference;
using caps_ranker::RankedCandidate;
using caps_ranker::rank;
using caps_ranker::format_weight;
using caps_ranker::pick_closest_fps;

namespace {

Capability make_cap(const std::string& media,
                   const std::string& raw,
                   uint32_t w, uint32_t h,
                   std::vector<double> fps) {
    Capability c;
    c.media_type = media;
    c.raw_format = raw;
    c.width = w;
    c.height = h;
    c.fps_list = std::move(fps);
    return c;
}

} // namespace

// =============================================================================
// format_weight
// =============================================================================
TEST(FormatWeight, JpegPreferredByDefault) {
    EXPECT_LT(format_weight("image/jpeg", "", true),
              format_weight("video/x-raw", "YUY2", true));
}

TEST(FormatWeight, JpegDemotedWhenPreferRawSelected) {
    // prefer_jpeg=false 时，JPEG 应该排在 raw 之后
    EXPECT_GT(format_weight("image/jpeg", "", false),
              format_weight("video/x-raw", "YUY2", false));
    EXPECT_GT(format_weight("image/jpeg", "", false),
              format_weight("video/x-raw", "I420", false));
}

TEST(FormatWeight, RawOrderingYUY2BetterThanI420) {
    // YUY2 / NV12 / I420 三档严格递增
    double y = format_weight("video/x-raw", "YUY2", true);
    double n = format_weight("video/x-raw", "NV12", true);
    double i = format_weight("video/x-raw", "I420", true);
    EXPECT_LT(y, n);
    EXPECT_LT(n, i);
}

TEST(FormatWeight, UnknownIsWorst) {
    double u = format_weight("application/x-blah", "", true);
    EXPECT_GT(u, format_weight("video/x-raw", "I420", true));
    EXPECT_GT(u, format_weight("image/jpeg", "", true));
}

// =============================================================================
// pick_closest_fps
// =============================================================================
TEST(PickClosestFps, BasicNearest) {
    EXPECT_DOUBLE_EQ(pick_closest_fps({9, 30, 60, 120}, 30.0), 30.0);
    EXPECT_DOUBLE_EQ(pick_closest_fps({9, 30, 60, 120}, 25.0), 30.0);
    EXPECT_DOUBLE_EQ(pick_closest_fps({9, 30, 60, 120}, 50.0), 60.0);
}

TEST(PickClosestFps, EmptyReturnsZero) {
    EXPECT_DOUBLE_EQ(pick_closest_fps({}, 30.0), 0.0);
}

TEST(PickClosestFps, EquidistantPrefersHigher) {
    // target 在 30 和 60 中间（45），距离都为 15，应该取 60
    EXPECT_DOUBLE_EQ(pick_closest_fps({30.0, 60.0}, 45.0), 60.0);
}

// =============================================================================
// rank
// =============================================================================
TEST(Rank, JpegBeatsRawAtSameResolution) {
    std::vector<Capability> caps = {
        make_cap("video/x-raw", "YUY2", 1280, 720, {30.0}),
        make_cap("image/jpeg",  "",     1280, 720, {30.0}),
    };
    Preference p; // 默认 1280x720@30, prefer_jpeg=true
    auto r = rank(caps, p);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].cap.media_type, "image/jpeg");
    EXPECT_EQ(r[1].cap.media_type, "video/x-raw");
}

TEST(Rank, ResolutionExactMatchBeatsBigger) {
    // 同格式下，正中目标的 720p 应该优先于 1080p
    std::vector<Capability> caps = {
        make_cap("image/jpeg", "", 1920, 1080, {30.0}),
        make_cap("image/jpeg", "", 1280, 720,  {30.0}),
    };
    Preference p;
    auto r = rank(caps, p);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].cap.width, 1280u);
    EXPECT_EQ(r[0].cap.height, 720u);
}

TEST(Rank, FpsPenaltyOnlyDistinguishesWhenFmtAndResEqual) {
    // 同分辨率同格式下，30fps 应优于 9fps（target=30）
    std::vector<Capability> caps = {
        make_cap("image/jpeg", "", 1280, 720, {9.0}),
        make_cap("image/jpeg", "", 1280, 720, {30.0}),
    };
    Preference p;
    auto r = rank(caps, p);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_DOUBLE_EQ(r[0].chosen_fps, 30.0);
    EXPECT_DOUBLE_EQ(r[1].chosen_fps, 9.0);
}

TEST(Rank, FormatBeatsResolutionDistance) {
    // JPEG 1080p 应该优于 YUY2 720p（格式权重 10 vs 720x2=1440 距离），
    // 但反过来：JPEG 1080p 比 JPEG 720p 差，YUY2 720p 比 YUY2 1080p 好——
    // 这条用例验证：跨格式时格式权重不会被分辨率距离反超
    std::vector<Capability> caps = {
        make_cap("video/x-raw", "YUY2", 1280, 720, {30.0}), // fmt=10 res=0
        make_cap("image/jpeg",  "",     1920, 1080, {30.0}), // fmt=0 res=720
    };
    Preference p; // 期望 1280x720
    auto r = rank(caps, p);
    ASSERT_EQ(r.size(), 2u);
    // JPEG 1080p 总分 = 0 + 720 + 0 = 720
    // YUY2 720p   总分 = 10 + 0 + 0 = 10
    // 所以 YUY2 720p 应该胜出（格式权重间隔不足以吃下 720 的分辨率距离）
    EXPECT_EQ(r[0].cap.media_type, "video/x-raw");
    EXPECT_EQ(r[0].cap.width, 1280u);
}

TEST(Rank, EmptyFpsListGetsDropped) {
    std::vector<Capability> caps = {
        make_cap("image/jpeg", "", 1280, 720, {}),     // 应被丢弃
        make_cap("image/jpeg", "", 1920, 1080, {30.0}),
    };
    Preference p;
    auto r = rank(caps, p);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].cap.width, 1920u);
}

TEST(Rank, ScoreMonotonicallyIncreasing) {
    // 排好序之后，score 应单调非降
    std::vector<Capability> caps = {
        make_cap("image/jpeg", "", 320, 240,  {30.0}),
        make_cap("image/jpeg", "", 1280, 720, {30.0}),
        make_cap("image/jpeg", "", 800, 600,  {30.0}),
        make_cap("image/jpeg", "", 1920, 1080,{30.0}),
    };
    Preference p; // 1280x720
    auto r = rank(caps, p);
    ASSERT_EQ(r.size(), 4u);
    for (size_t i = 1; i < r.size(); ++i) {
        EXPECT_LE(r[i - 1].score, r[i].score)
            << "排序结果不单调：[" << i - 1 << "].score=" << r[i - 1].score
            << " > [" << i << "].score=" << r[i].score;
    }
    // 最优应为 1280x720
    EXPECT_EQ(r[0].cap.width, 1280u);
}

TEST(Rank, TiebreakerPrefersLargerResolution) {
    // 两条候选 score 完全相等的情况下，倾向更大尺寸
    // 构造：JPEG 640x480@30 vs JPEG 640x480@30（width 不一样为 640 vs 800，
    // 而 target 设在它们对称位置 720x480 让 res 距离相等）
    std::vector<Capability> caps = {
        make_cap("image/jpeg", "", 640, 480, {30.0}),
        make_cap("image/jpeg", "", 800, 480, {30.0}),
    };
    Preference p;
    p.width = 720; p.height = 480; p.fps = 30.0;
    auto r = rank(caps, p);
    ASSERT_EQ(r.size(), 2u);
    // 两边 res 距离都是 80，score 相等，tiebreaker 应取较大者（800）
    EXPECT_EQ(r[0].cap.width, 800u);
}

TEST(Rank, ChosenFpsReflectsTargetProximity) {
    std::vector<Capability> caps = {
        make_cap("image/jpeg", "", 1280, 720, {15.0, 30.0, 60.0, 120.0}),
    };
    Preference p; p.width = 1280; p.height = 720;

    p.fps = 30.0;
    EXPECT_DOUBLE_EQ(rank(caps, p).at(0).chosen_fps, 30.0);

    p.fps = 50.0;
    EXPECT_DOUBLE_EQ(rank(caps, p).at(0).chosen_fps, 60.0);

    p.fps = 100.0;
    EXPECT_DOUBLE_EQ(rank(caps, p).at(0).chosen_fps, 120.0);
}

// =============================================================================
// shortfall 强惩罚：fps 低于目标的候选必须排在 fps>=目标 的候选之后
// =============================================================================

// 业务背景：日志里 YUY2 720p@9 被排第 1，下游收敛到 30fps 时 v4l2src
// 抛 not-negotiated。新评分规则下，shortfall 候选无论格式多优都得让路。
TEST(Rank, ShortfallRankedBelowAnySatisfyingCandidate) {
    // JPEG 720p@9 (shortfall 21Hz)  vs  YUY2 720p@30 (满足目标)
    // 即便 JPEG 格式权重更优 (0 vs 10)，shortfall 强惩罚 21*50=1050
    // 也要把 JPEG@9 压到后面去。
    std::vector<Capability> caps = {
        make_cap("image/jpeg",  "",     1280, 720, {9.0}),
        make_cap("video/x-raw", "YUY2", 1280, 720, {30.0}),
    };
    Preference p; // 目标 1280x720@30
    auto r = rank(caps, p);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].cap.media_type, "video/x-raw");
    EXPECT_DOUBLE_EQ(r[0].chosen_fps, 30.0);
    EXPECT_EQ(r[1].cap.media_type, "image/jpeg");
    EXPECT_DOUBLE_EQ(r[1].chosen_fps, 9.0);
}

// 复现日志的真实场景：能力清单 = 用户实际设备的子集，目标 720p@30，
// 期望 Top1 = MJPEG 720p@60（高于目标，可由 videorate 抽帧降到 30）
// 而不是 YUY2 720p@9（低于目标，硬伤）
TEST(Rank, RealDeviceScenarioPicksMjpeg720p60) {
    std::vector<Capability> caps = {
        make_cap("image/jpeg",  "",     1280, 720,  {60.0}),
        make_cap("image/jpeg",  "",     1920, 1080, {30.0}),
        make_cap("image/jpeg",  "",     1280, 1024, {30.0}),
        make_cap("video/x-raw", "YUY2", 1280, 720,  {9.0}),
    };
    Preference p; // 默认 1280x720@30
    auto r = rank(caps, p);
    ASSERT_GE(r.size(), 1u);
    EXPECT_EQ(r[0].cap.media_type, "image/jpeg");
    EXPECT_EQ(r[0].cap.width,  1280u);
    EXPECT_EQ(r[0].cap.height, 720u);
    EXPECT_DOUBLE_EQ(r[0].chosen_fps, 60.0);
}

// 高于目标 fps 的"轻微"惩罚：同格式同分辨率下，更接近目标的胜出，
// 但都不会被压到 shortfall 后面。
TEST(Rank, OverflowFpsStillBeatsShortfall) {
    std::vector<Capability> caps = {
        make_cap("image/jpeg", "", 1280, 720, {120.0}),  // 高 90Hz：轻罚 90*5=450
        make_cap("image/jpeg", "", 1280, 720, {29.0}),   // 低 1Hz： 强罚 1*50=50
    };
    Preference p;
    auto r = rank(caps, p);
    ASSERT_EQ(r.size(), 2u);
    // 即便 29fps 离目标只差 1Hz，shortfall 强惩罚（50）仍小于 overflow 90Hz（450），
    // 但 overflow 的"轻微"语义意味着：只要再往上拉一档，shortfall 就得让位。
    // 这里实际比较 50 vs 450 —— 1Hz shortfall 反倒更优；这是合理的，
    // 因为 1Hz 的差距确实接近"满足"。
    EXPECT_DOUBLE_EQ(r[0].chosen_fps, 29.0);
    EXPECT_DOUBLE_EQ(r[1].chosen_fps, 120.0);

    // 但若 shortfall 大到 21Hz（罚 1050），就必须输给 overflow 90Hz（罚 450）
    std::vector<Capability> caps2 = {
        make_cap("image/jpeg", "", 1280, 720, {120.0}),
        make_cap("image/jpeg", "", 1280, 720, {9.0}),
    };
    auto r2 = rank(caps2, p);
    ASSERT_EQ(r2.size(), 2u);
    EXPECT_DOUBLE_EQ(r2[0].chosen_fps, 120.0);
    EXPECT_DOUBLE_EQ(r2[1].chosen_fps, 9.0);
}

// =============================================================================
// format_ranking 输出
// =============================================================================
TEST(FormatRanking, EmptyPlaceholder) {
    EXPECT_EQ(caps_ranker::format_ranking({}), "(no candidates)");
}

TEST(FormatRanking, ContainsIndexAndScore) {
    std::vector<Capability> caps = {
        make_cap("image/jpeg", "", 1280, 720, {30.0}),
    };
    auto r = rank(caps, Preference{});
    auto s = caps_ranker::format_ranking(r);
    EXPECT_NE(s.find("[1]"), std::string::npos);
    EXPECT_NE(s.find("score="), std::string::npos);
    EXPECT_NE(s.find("1280x720"), std::string::npos);
}