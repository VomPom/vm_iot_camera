

//
// Created by vompom on 2026/06/12.
//
// @Description
//   V4L2Prober 实现：核心是三层 ioctl 嵌套枚举。
//   纯函数 map_pixelformat / intervals_to_fps 与 ioctl 无关，
//   单测可以独立驱动；probe() 自身依赖真实设备，归集成测试。
//

#include "v4l2_prober.h"

#include "log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <sstream>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace v4l2_prober {

// ----------------------------------------------------------------------------
// Capability::to_string
// ----------------------------------------------------------------------------
std::string Capability::to_string() const {
    std::ostringstream oss;
    if (!raw_format.empty()) {
        oss << media_type << "(" << raw_format << ")";
    } else {
        oss << media_type;
    }
    oss << " " << width << "x" << height << " @[";
    for (size_t i = 0; i < fps_list.size(); ++i) {
        if (i) oss << ",";
        // 保留一位小数即可，120.101 这种少数情况会被四舍五入到 120.1
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f", fps_list[i]);
        oss << buf;
    }
    oss << "]";
    return oss.str();
}

// ----------------------------------------------------------------------------
// map_pixelformat：fourcc → GStreamer media_type
// ----------------------------------------------------------------------------
//
// 仅覆盖 USB / 板载摄像头常见格式。冷门格式（GREY/Y10/Y16/RGB565/...）不在
// 表里时返回 {"",""}，由调用方按需丢弃。这样比硬编码大表更易维护。
//
// 注意：表里的 fourcc 字面量直接用 v4l2 头文件宏，避免拼错。在非 Linux 平台
// 下 V4L2_PIX_FMT_* 不可用，所以我们用 v4l2_fourcc 等价宏自己拼一份。
namespace {

// V4L2 fourcc 编码方式：4 个 ASCII 字符按小端打包成 uint32_t。
// 这是 v4l2.h 里 v4l2_fourcc 宏的实现，自己重写一份以脱离头文件。
constexpr uint32_t make_fourcc(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(a))
         | (static_cast<uint32_t>(b) << 8)
         | (static_cast<uint32_t>(c) << 16)
         | (static_cast<uint32_t>(d) << 24);
}

struct FmtRow {
    uint32_t fourcc;
    const char* media_type;
    const char* raw_format;
};

// 常见格式表，按"摄像头出现频率"从高到低排（仅文档作用，查找用 unordered_map）。
const std::array<FmtRow, 8> kFmtTable{{
    { make_fourcc('M','J','P','G'), "image/jpeg",   ""     },
    { make_fourcc('J','P','E','G'), "image/jpeg",   ""     },
    { make_fourcc('Y','U','Y','V'), "video/x-raw",  "YUY2" }, // GStreamer 把 V4L2 的 YUYV 叫 YUY2
    { make_fourcc('U','Y','V','Y'), "video/x-raw",  "UYVY" },
    { make_fourcc('N','V','1','2'), "video/x-raw",  "NV12" },
    { make_fourcc('N','V','2','1'), "video/x-raw",  "NV21" },
    { make_fourcc('Y','U','1','2'), "video/x-raw",  "I420" }, // V4L2 YU12 == GStreamer I420
    { make_fourcc('Y','V','1','2'), "video/x-raw",  "YV12" },
}};

} // namespace

MediaTypeMapping map_pixelformat(uint32_t fourcc) {
    for (const auto& row : kFmtTable) {
        if (row.fourcc == fourcc) {
            return { row.media_type, row.raw_format };
        }
    }
    return { "", "" };
}

// ----------------------------------------------------------------------------
// intervals_to_fps：把 V4L2 frameinterval 转成 fps 列表
// ----------------------------------------------------------------------------
//
// V4L2 用"interval = 1/fps"表示帧率，所以拿到的是 numerator/denominator 形式
// 的时间间隔（秒）。fps = denominator / numerator。
//
// - DISCRETE: 一个具体值，直接换算。
// - STEPWISE: [min, max] 区间 + step。按 step 采样，封顶 16 点，避免厂商把
//   step 设成 1/1000000 之类导致几百万个点。
// - CONTINUOUS: 等价于 step==1（任意值），同样按 16 点等距采样。
//
// 所有 fps 都四舍五入到一位小数对齐，便于下游评分时去重。
std::vector<double> intervals_to_fps(const struct v4l2_frmivalenum& base) {
    std::vector<double> result;

    auto interval_to_fps = [](uint32_t num, uint32_t den) -> double {
        if (num == 0) return 0.0; // 防御：理论上 V4L2 不会给出 0 分子
        return static_cast<double>(den) / static_cast<double>(num);
    };

    auto round1 = [](double v) {
        return std::round(v * 10.0) / 10.0;
    };

    if (base.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
        result.push_back(round1(interval_to_fps(base.discrete.numerator,
                                                base.discrete.denominator)));
        return result;
    }

    // STEPWISE / CONTINUOUS：把 [min, max] 间隔映射到 [fps_at_max, fps_at_min]。
    // 注意 fps 与 interval 反向：interval 越大 fps 越小，所以 fps 区间是
    // [fps_min = den_max/num_max（用 max interval）, fps_max = den_min/num_min（用 min interval）]。
    const double fps_lo = interval_to_fps(base.stepwise.max.numerator,
                                          base.stepwise.max.denominator);
    const double fps_hi = interval_to_fps(base.stepwise.min.numerator,
                                          base.stepwise.min.denominator);
    if (fps_lo <= 0 || fps_hi <= 0) return result;

    // 等距采样，最多 16 个点
    constexpr int kMaxSamples = 16;
    int n = kMaxSamples;
    if (fps_hi - fps_lo < 1e-6) n = 1; // 区间退化成一点
    for (int i = 0; i < n; ++i) {
        double t = (n == 1) ? 0.0 : static_cast<double>(i) / (n - 1);
        double fps = fps_lo + (fps_hi - fps_lo) * t;
        result.push_back(round1(fps));
    }

    // 去重（采样后可能有重复，比如 [29.97, 30] 被 round1 拍平到 30.0）
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    return result;
}

// ----------------------------------------------------------------------------
// probe：仅 Linux 实现，三层 ioctl 嵌套枚举
// ----------------------------------------------------------------------------
#if defined(__linux__)

namespace {

/// 安全 ioctl：自动重试 EINTR
int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do {
        r = ::ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

/// 给定 (fd, fmt, w, h) 枚举所有支持的帧率
std::vector<double> enum_framerates(int fd, uint32_t fourcc, uint32_t w, uint32_t h) {
    std::vector<double> all_fps;
    for (uint32_t idx = 0; ; ++idx) {
        struct v4l2_frmivalenum fival{};
        fival.index = idx;
        fival.pixel_format = fourcc;
        fival.width = w;
        fival.height = h;
        if (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fival) == -1) {
            // 第一次就 EINVAL 表示该 (fmt,w,h) 没注册帧率枚举，跳过
            break;
        }
        auto fps = intervals_to_fps(fival);
        all_fps.insert(all_fps.end(), fps.begin(), fps.end());

        // STEPWISE/CONTINUOUS 只有一条记录，DISCRETE 才会循环递增 index
        if (fival.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
    }
    // 去重并按从大到小排（高帧率优先曝露给上层）
    std::sort(all_fps.begin(), all_fps.end(), std::greater<double>());
    all_fps.erase(std::unique(all_fps.begin(), all_fps.end()), all_fps.end());
    return all_fps;
}

} // namespace

std::vector<Capability> probe(const std::string& device) {
    std::vector<Capability> caps;

    int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        LOGW("v4l2_prober: open({}) failed: {}", device, std::strerror(errno));
        return caps;
    }

    // Layer 1: 枚举像素格式
    for (uint32_t fmt_idx = 0; ; ++fmt_idx) {
        struct v4l2_fmtdesc fmt{};
        fmt.index = fmt_idx;
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &fmt) == -1) break;

        auto mapping = map_pixelformat(fmt.pixelformat);
        if (mapping.media_type.empty()) {
            // 未知格式跳过：这次只列我们能让 GStreamer 正确处理的
            LOGW("v4l2_prober: skip unknown fourcc 0x{:08x} ('{}')",
                 fmt.pixelformat, reinterpret_cast<const char*>(fmt.description));
            continue;
        }

        // Layer 2: 枚举该格式下的所有分辨率
        for (uint32_t fs_idx = 0; ; ++fs_idx) {
            struct v4l2_frmsizeenum fs{};
            fs.index = fs_idx;
            fs.pixel_format = fmt.pixelformat;
            if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == -1) break;

            // 当前只处理 DISCRETE 分辨率（USB UVC 摄像头几乎全是这种）
            // STEPWISE/CONTINUOUS 分辨率（一些 ISP/CSI 设备）后续按需扩展
            if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
                LOGW("v4l2_prober: skip non-discrete framesize for fourcc 0x{:08x}",
                     fmt.pixelformat);
                break;
            }

            // Layer 3: 枚举帧率
            auto fps_list = enum_framerates(fd, fmt.pixelformat,
                                            fs.discrete.width, fs.discrete.height);

            Capability cap;
            cap.media_type   = mapping.media_type;
            cap.raw_format   = mapping.raw_format;
            cap.pixelformat  = fmt.pixelformat;
            cap.width        = fs.discrete.width;
            cap.height       = fs.discrete.height;
            cap.fps_list     = std::move(fps_list);
            caps.push_back(std::move(cap));
        }
    }

    ::close(fd);
    return caps;
}

#else // 非 Linux 平台

std::vector<Capability> probe(const std::string& device) {
    LOGW("v4l2_prober: probe('{}') is only supported on Linux, returning empty", device);
    return {};
}

#endif

// ----------------------------------------------------------------------------
// format_table：友好打印整张能力清单
// ----------------------------------------------------------------------------
std::string format_table(const std::vector<Capability>& caps) {
    if (caps.empty()) return "(no capabilities)";

    // 按 media_type+格式分组打印，对运维更友好
    // 输出示例：
    //   [image/jpeg]
    //     1280x720 @ 60.0 fps
    //     1920x1080 @ 30.0 fps
    //   [video/x-raw / YUY2]
    //     640x480 @ 30.0 fps
    std::ostringstream oss;
    std::string current_group;

    // 先按 media_type / raw_format 稳定排序，纯展示用，不影响真实顺序
    std::vector<size_t> idx(caps.size());
    for (size_t i = 0; i < caps.size(); ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        const auto& ca = caps[a];
        const auto& cb = caps[b];
        if (ca.media_type != cb.media_type) return ca.media_type < cb.media_type;
        if (ca.raw_format != cb.raw_format) return ca.raw_format < cb.raw_format;
        if (ca.width != cb.width) return ca.width < cb.width;
        return ca.height < cb.height;
    });

    for (size_t k : idx) {
        const auto& c = caps[k];
        std::string group = c.raw_format.empty()
            ? ("[" + c.media_type + "]")
            : ("[" + c.media_type + " / " + c.raw_format + "]");
        if (group != current_group) {
            if (!current_group.empty()) oss << "\n";
            oss << group << "\n";
            current_group = group;
        }
        oss << "  " << c.width << "x" << c.height << " @ ";
        for (size_t i = 0; i < c.fps_list.size(); ++i) {
            if (i) oss << ", ";
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1f", c.fps_list[i]);
            oss << buf;
        }
        oss << " fps\n";
    }
    return oss.str();
}

} // namespace v4l2_prober
