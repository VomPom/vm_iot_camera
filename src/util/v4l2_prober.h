//
// Created by vompom on 2026/06/12.
//
// @Description
//   V4L2 摄像头能力探测器：通过 ioctl 三层枚举（VIDIOC_ENUM_FMT →
//   VIDIOC_ENUM_FRAMESIZES → VIDIOC_ENUM_FRAMEINTERVALS）拿到设备
//   支持的 (像素格式, 分辨率, 帧率) 全集，输出与 GStreamer caps 对齐
//   的 Capability 列表，供上层 CapsRanker 评分排序使用。
//
//   仅 Linux 可用。非 Linux 平台 probe() 直接返回空表并打印 warning，
//   不影响主程序构建。
//
//   TODO(libcamera): 后续接入树莓派 CSI 摄像头时，可在同目录新增
//   libcamera_prober.{h,cpp} 暴露相同的 probe()/Capability 接口，
//   由 PipelineBuilder 按 device 路径前缀（/dev/video* vs CSI）分派。
//

#ifndef VM_IOT_V4L2_PROBER_H
#define VM_IOT_V4L2_PROBER_H

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 仅在真实 V4L2 头文件可用时引入，否则 intervals_to_fps 用前向声明的兼容结构体。
// 保证 macOS 上单元测试也能编译。
#if defined(__linux__)
#include <linux/videodev2.h>
#else
// macOS / 其他平台：定义最小兼容的占位结构体，仅供单元测试编译期使用。
// 字段命名严格对齐 linux/videodev2.h，便于代码两端共享。
struct v4l2_fract {
    uint32_t numerator;
    uint32_t denominator;
};

struct v4l2_frmival_stepwise {
    struct v4l2_fract min;
    struct v4l2_fract max;
    struct v4l2_fract step;
};

struct v4l2_frmivalenum {
    uint32_t index;
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint32_t type; // 1=DISCRETE, 2=CONTINUOUS, 3=STEPWISE
    union {
        struct v4l2_fract             discrete;
        struct v4l2_frmival_stepwise  stepwise;
    };
    uint32_t reserved[2];
};

// 与 V4L2 头文件保持一致的常量
#ifndef V4L2_FRMIVAL_TYPE_DISCRETE
#define V4L2_FRMIVAL_TYPE_DISCRETE   1
#define V4L2_FRMIVAL_TYPE_CONTINUOUS 2
#define V4L2_FRMIVAL_TYPE_STEPWISE   3
#endif
#endif // __linux__

namespace v4l2_prober {
    struct Capability;

/**
 * probe / device_accessible 的错误分类。
 *
 * 引入原因：运行期 UVC 摄像头可能被拔除，`open()` 返回的 errno 需要区分：
 *   - ENODEV / ENOENT → 设备真的不在了，属于"预期内"事件（拔除或从未插入），
 *     日志级别用 warn 即可，下一次 client DESCRIBE 会由 rtsp-server 懒重建。
 *   - EBUSY           → 设备在，只是被其他进程占用。绝不能当作"拔除"处理，
 *     否则会引发不必要的 pipeline 重建风暴。
 *   - EACCES          → 权限问题，人为运维错误，需要 error 级并保留原始 errno。
 *   - 其它            → 未知，保守当作 Unknown。
 *
 * 非 Linux 平台一律返回 NotSupported。
 */
enum class ProbeStatus {
    Ok,             //< 打开成功且拿到至少一条能力（或 device_accessible 通过）
    NoDevice,       //< ENODEV / ENOENT：设备节点不存在（含被拔除中间态）
    Busy,           //< EBUSY：被其他进程占用
    Permission,     //< EACCES：权限不足
    OpenFailed,     //< 其它 open() 失败
    NoCapability,   //< open 成功但 ioctl 枚举出 0 条可用能力（罕见）
    NotSupported,   //< 非 Linux 平台
};

// 探测结果：状态 + 能力列表 + 原始 errno（Linux 上打开失败时有效，其它场景为 0）。
struct ProbeResult {
    /* 默认 OpenFailed：调用方在未真正调用 probe_ex() 拿到结果前，一律按"失败"处理，
     * 避免"忘了填字段"变成"看起来一切正常"的伪造成功。 */
    ProbeStatus              status = ProbeStatus::OpenFailed;
    std::vector<Capability>  caps;
    int                      last_errno = 0;   //< open() 失败时保留 errno，便于日志/上层判断

    bool ok() const { return status == ProbeStatus::Ok; }
    // 语义：设备"物理不在"（拔除或从未插入）。
    bool device_absent() const {
        return status == ProbeStatus::NoDevice;
    }
};

// ProbeStatus 的可读名（日志用）。
const char* to_string(ProbeStatus s);

/**
 * 设备一条能力记录：一个 (pixelformat, 分辨率) 组合 + 该组合下支持的所有帧率。
 *
 * 之所以把同一分辨率下的多帧率聚成一条而不是展开成多条，是因为后续 ranker 需要
 * 在该分辨率内挑选"最接近期望帧率"的那个 fps，把候选数量收敛掉。
 */
struct Capability {
    // GStreamer media type，对应 caps 字符串的开头：
    // "image/jpeg" / "video/x-raw"。未知格式时为空。
    std::string media_type;

    // 当 media_type == "video/x-raw" 时的格式字段，如 "YUY2"/"NV12"/"I420"。
    // 当 media_type == "image/jpeg" 时为空。
    std::string raw_format;

    // V4L2 fourcc 原始值，调试和日志用，便于和 v4l2-ctl 输出对齐。
    uint32_t pixelformat = 0;

    uint32_t width  = 0;
    uint32_t height = 0;

    // 该 (fmt,w,h) 支持的所有 fps。
    // - DISCRETE：直接按枚举到的离散值列出。
    // - STEPWISE/CONTINUOUS：按 step 采样，最多 16 个点，避免无限展开。
    std::vector<double> fps_list;

    // 友好打印（单行），形如：
    //   image/jpeg 1280x720 @[60.0]
    std::string to_string() const;
};

/**
 * 真实探测设备能力。
 *
 * @param device V4L2 设备路径，例如 "/dev/video0"。
 * @return 能力清单。设备打不开 / 非 Linux 平台 / 无可用格式时返回空向量，
 *         不抛异常（错误信息会通过 LOGW 打印）。
 */
std::vector<Capability> probe(const std::string& device);

/**
 * 结构化探测：等价于 probe()，但额外返回错误分类和 errno。
 *
 * 用于运行期热拔插逻辑区分
 * "设备被拔了" vs "被占用" vs "权限错" 三类不同处理路径。
 *
 * 语义等价规则：
 *   - probe_ex().status == Ok    ⇔ probe() 返回 非空 caps
 *   - probe_ex().status == NoCapability ⇔ probe() 返回 空 caps 但 open 成功
 *   - 其它 status                ⇔ probe() 返回空 caps 且日志已打印失败原因
 */
ProbeResult probe_ex(const std::string& device);

/**
 * 快速检测设备是否存在且可访问（仅 open(O_RDWR) 验证，不做 ioctl 探测）。
 * 用于启动期前置检查：设备没插/权限不够直接给人类可读错误，而非等 RTSP 客户端
 * 连上来才看到一句模糊的 503。
 *
 * @return true=设备文件存在且可读写打开；false=不存在/无权限/非 Linux 平台。
 */
bool device_accessible(const std::string& device);

/**
 * device_accessible 的带错误分类重载：
 *   - out_status 非空时写入 ProbeStatus（Ok/NoDevice/Busy/Permission/OpenFailed/NotSupported）
 *   - out_errno  非空时写入 open() 失败的 errno（成功或非 Linux 时置 0）
 * 主要给运行期热拔插逻辑区分错误类型时使用。
 */
bool device_accessible(const std::string& device,
                       ProbeStatus*       out_status,
                       int*               out_errno);

/**
 * 把整张能力表格式化成多行文本，风格类似 `v4l2-ctl --list-formats-ext`。
 * 用于启动期日志，方便运维排错。
 */
std::string format_table(const std::vector<Capability>& caps);

// ============================================================================
// 以下纯函数仅为单元测试暴露，不建议生产代码直接调用。
// ============================================================================

// V4L2 fourcc → GStreamer media_type / raw_format 的映射结果
struct MediaTypeMapping {
    std::string media_type;   //< "image/jpeg" / "video/x-raw" / ""（未知）
    std::string raw_format;   //< "YUY2"/"NV12"/...（jpeg 或未知时为空）
};

// 把 V4L2 fourcc 转换成 GStreamer caps 用的 media_type + raw_format。
// 未知 fourcc 返回 {"", ""}，由调用方决定丢弃还是兜底。
MediaTypeMapping map_pixelformat(uint32_t fourcc);

// 把 V4L2 frameinterval 枚举结果转换为 fps 列表。
// 处理 DISCRETE / STEPWISE / CONTINUOUS 三种类型，对 step 步进做封顶采样。
std::vector<double> intervals_to_fps(const struct v4l2_frmivalenum& base);

} // namespace v4l2_prober

#endif // VM_IOT_V4L2_PROBER_H