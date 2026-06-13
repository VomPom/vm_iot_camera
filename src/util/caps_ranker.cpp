//
// Created by vompom on 2026/06/13.
//
// CapsRanker 实现：纯函数，无 IO 依赖。
//

#include "caps_ranker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace caps_ranker {

// ----------------------------------------------------------------------------
// 评分常量集中管理
// ----------------------------------------------------------------------------
//
// 设计原则：
//   - 格式权重间隔留够（10/20/30/100），避免分辨率/fps 的小差距把"格式偏好"
//     这一首要原则推翻。例如：JPEG 720p (fmt=0, res=0) 必须比 YUY2 720p
//     (fmt=10, res=0) 排得更靠前。
//   - kFpsPenaltyPerHz（5）比格式间距小一个量级，让"差 1 fps"不会跨格式反超；
//     但当分辨率/格式都相同时，fps 差异能区分候选。
//   - kFpsPenaltyPerHzShortfall（50）专门惩罚"低于目标 fps"的候选：上采样有损，
//     且 v4l2src 实际只能给到 N fps 时下游强写更高 fps 常常 not-negotiated。
namespace {

constexpr double kFmtJpeg     = 0.0;
constexpr double kFmtYUY2     = 10.0;
constexpr double kFmtNV12     = 20.0;
constexpr double kFmtI420     = 30.0;
constexpr double kFmtRawOther = 50.0;
constexpr double kFmtUnknown  = 100.0;

/// 当 prefer_jpeg=false 时把 JPEG 罚到 raw 之后但仍优于 unknown。
constexpr double kFmtJpegLow  = 60.0;

// fps 评分系数：区分“高于目标”与“低于目标”两种情况。
//   - 高于目标 fps：videorate 抽帧无损，仅做轻微惩罚（与原来一致），
//     用于把 240fps 这类极端值往期望帧率附近收敛。
//   - 低于目标 fps：videorate 上采样有损（复制帧），且 v4l2src 实际就这速度，
//     下游统一收敛到目标 fps 时常出现 not-negotiated。必须强惩罚把这类候选
//     压到“任何满足 fps>=目标”的候选之后。系数取 50（约等于 5 个格式档位的差距），
//     保证哪怕是 unknown 格式 + 满足 fps 的候选，也优于 JPEG + shortfall 1Hz 的候选。
constexpr double kFpsPenaltyPerHz          = 5.0;
constexpr double kFpsPenaltyPerHzShortfall = 50.0;

} // namespace
// ----------------------------------------------------------------------------
double format_weight(const std::string& media_type,
                     const std::string& raw_format,
                     bool prefer_jpeg) {
    if (media_type == "image/jpeg") {
        return prefer_jpeg ? kFmtJpeg : kFmtJpegLow;
    }
    if (media_type == "video/x-raw") {
        if (raw_format == "YUY2" || raw_format == "UYVY") return kFmtYUY2;
        if (raw_format == "NV12" || raw_format == "NV21") return kFmtNV12;
        if (raw_format == "I420" || raw_format == "YV12") return kFmtI420;
        return kFmtRawOther; // 已识别为 raw 但不在常用表
    }
    return kFmtUnknown;
}

// ----------------------------------------------------------------------------
// pick_closest_fps
// ----------------------------------------------------------------------------
double pick_closest_fps(const std::vector<double>& fps_list, double target) {
    if (fps_list.empty()) return 0.0;

    double best     = fps_list.front();
    double best_dst = std::fabs(best - target);
    for (size_t i = 1; i < fps_list.size(); ++i) {
        double f   = fps_list[i];
        double dst = std::fabs(f - target);
        if (dst < best_dst) {
            best     = f;
            best_dst = dst;
        } else if (std::fabs(dst - best_dst) < 1e-6 && f > best) {
            // 等距时优先取较大者（高帧率优先）
            best = f;
        }
    }
    return best;
}

// ----------------------------------------------------------------------------
// rank
// ----------------------------------------------------------------------------
std::vector<RankedCandidate> rank(
    const std::vector<v4l2_prober::Capability>& caps,
    const Preference& pref) {

    std::vector<RankedCandidate> out;
    out.reserve(caps.size());

    for (const auto& c : caps) {
        if (c.fps_list.empty()) {
            // 没有可用 fps：跳过。一些早期 V4L2 驱动会出现这种情况，
            // 强行取就只能用 0 fps，下游 GStreamer 也拼不出 caps。
            continue;
        }

        RankedCandidate r;
        r.cap        = c;
        r.chosen_fps = pick_closest_fps(c.fps_list, pref.fps);

        r.score_format     = format_weight(c.media_type, c.raw_format, pref.prefer_jpeg);
        r.score_resolution = std::fabs(static_cast<double>(c.width)  - pref.width)
                           + std::fabs(static_cast<double>(c.height) - pref.height);

        // fps 评分：区分“高于目标 fps”（可降采样）与“低于目标 fps”（硬伤）。
        // shortfall 用大系数强惩罚，避免 9fps 这类候选反超 60fps。
        const double fps_diff = r.chosen_fps - pref.fps;
        r.score_fps = (fps_diff >= 0.0)
                          ? fps_diff  * kFpsPenaltyPerHz
                          : -fps_diff * kFpsPenaltyPerHzShortfall;
        r.score = r.score_format + r.score_resolution + r.score_fps;

        out.push_back(std::move(r));
    }

    // 按 score 升序，等分时按 (width,height,chosen_fps) 倒序作 tiebreaker
    // 直觉：分一样时，宁可拿大分辨率/高帧率的，因为评分函数本身已经把"过分"
    // 的尺寸惩罚掉了；剩下打平的多为期望参数附近的几个邻近候选。
    std::stable_sort(out.begin(), out.end(),
        [](const RankedCandidate& a, const RankedCandidate& b) {
            if (std::fabs(a.score - b.score) > 1e-6) return a.score < b.score;
            if (a.cap.width  != b.cap.width)  return a.cap.width  > b.cap.width;
            if (a.cap.height != b.cap.height) return a.cap.height > b.cap.height;
            return a.chosen_fps > b.chosen_fps;
        });

    return out;
}

// ----------------------------------------------------------------------------
// to_string / format_ranking
// ----------------------------------------------------------------------------
std::string RankedCandidate::to_string() const {
    std::ostringstream oss;
    if (!cap.raw_format.empty()) {
        oss << cap.media_type << "(" << cap.raw_format << ")";
    } else {
        oss << cap.media_type;
    }
    oss << " " << cap.width << "x" << cap.height;

    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  " @%.1f score=%.1f (fmt=%.1f res=%.1f fps=%.1f)",
                  chosen_fps, score, score_format, score_resolution, score_fps);
    oss << buf;
    return oss.str();
}

std::string format_ranking(const std::vector<RankedCandidate>& ranked) {
    if (ranked.empty()) return "(no candidates)";
    std::ostringstream oss;
    for (size_t i = 0; i < ranked.size(); ++i) {
        char idx[16];
        std::snprintf(idx, sizeof(idx), "[%zu] ", i + 1);
        oss << idx << ranked[i].to_string();
        if (i + 1 < ranked.size()) oss << "\n";
    }
    return oss.str();
}

} // namespace caps_ranker
