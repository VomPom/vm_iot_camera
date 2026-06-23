//
// Created by vompom on on 2026/06/05 17:04.
//
// @Description
//   应用配置结构定义与 YAML 加载接口
//

#ifndef VM_IOT_CONFIG_H
#define VM_IOT_CONFIG_H

#include <string>
#include <cstdint>


struct ServerConfig {
    uint16_t port = 8554;
    std::string mount = "/live";
};

/*
 * 摄像头采集配置。
 *
 * 字段语义：
 *   - width / height / framerate：用户「期望」参数，用于在启动期与设备
 *     真实能力做评分匹配（V4L2Prober + CapsRanker）；不再要求设备必须
 *     精确支持这套参数。
 *   - pixfmt：编码器输入的统一格式（下游 caps），与上游探测解耦。
 *     上游 caps 由 ranker 选出（可能是 image/jpeg / YUY2 / NV12 等），
 *     之后必有 jpegdec? + videoconvert + videoscale + videorate 收敛到该 pixfmt。
 *   - prefer_jpeg：true 时 ranker 优先选 MJPG（USB 摄像头常用，码流小）；
 *     false 时优先选 raw（未来 zero-copy GPU 上传场景）。
 */
struct CaptureConfig {
    std::string device = "/dev/video0";
    int width = 1280;
    int height = 720;
    int framerate = 30;
    std::string pixfmt = "I420";
    bool prefer_jpeg = true;
};

struct EncoderConfig {
    std::string backend = "x264";
    int bitrate_kbps = 4000;
    int gop = 30;
    int bframes = 0;
};

struct LogConfig {
    std::string level = "info";
};

/* PAG 滤镜（自研 GStreamer 元素 pagfilter）的配置。
 *
 * 当前形态：enabled=true 时在 pipeline 中插入 pagfilter，并根据 file
 *           加载 .pag 资产、alpha-blend 到主线画面（运行期亦可热切）；
 *           file 为空时 pagfilter 仍在管线中，但退化为 passthrough，不修改像素。
 *           selftest=true 时启动期一次性调用 libpag 加载
 *           file 指向的 .pag 文件并打印元信息，证明 SDK 编译/链接通。
 *           selftest 独立于 enabled，即使 enabled=false 也会执行，
 *           便于在不影响 pipeline 的情况下单独验证 SDK。
 *
 * 默认 enabled=false，确保现网行为不变。 */
struct PagFilterConfig {
    bool        enabled  = false;                    // 总开关；false 时不在 pipeline 插入 pagfilter
    bool        selftest = false;                    // true 时启动期尝试加载 file 并打印元信息
    std::string file;                                // .pag 素材路径；相对路径以 config_dir/.. 为基目录
};

struct FilterConfig {
    bool        enabled     = true;
    std::string shader      = "effects.frag";        // 单一 shader 文件；运行时通过 uniform filter_type 切换分支
    int         filter_type = 0;                     // 启动默认特效：0=passthrough 1=mosaic 2=invert ...
    int         max_type    = 2;                     // filter next/prev 循环上限（含），与 shader 内 if 分支保持一致

    /* PAG 自研滤镜段，独立于上面的 GL shader 段：
     * 二者可以同时启用，pagfilter 会插在 GL 段之后、第一个 tee 之前。 */
    PagFilterConfig pag;
};

/* 控制通道（命令 FIFO）独立于 filter，承载所有运行时指令：
 *   filter / reload / status / snapshot ...
 * 与 filter 解耦后，filter.enabled=false 也能照常使用 status / snapshot。 */
struct ControlConfig {
    std::string request_fifo;  // 请求 FIFO 路径，空串=不开启控制通道
    std::string reply_fifo;    // 应答 FIFO 路径，空串=不写回执
};

/* 截图副线（snapshot branch）配置。 */
struct SnapshotConfig {
    std::string dir        = "/tmp/vm_iot/snapshots"; // 默认输出目录；take 命令未传 path 时使用
    int         quality    = 90;                      // JPEG 质量（预留，当前 jpegenc 未启用此项）
    int         timeout_ms = 1500;                    // 等待 buffer 落盘超时
};

/* 录像副线（record branch）配置。*/

struct Config {
    ServerConfig   server;
    CaptureConfig  capture;
    EncoderConfig  encoder;
    LogConfig      log;
    FilterConfig   filter;
    ControlConfig  control;
    SnapshotConfig snapshot;
    // TODO(record): 重开录像时恢复  RecordConfig record;

    std::string config_dir;

    static Config from_file(const std::string &path);

    void merge_cli(int argc, char **argv);
};

#endif //VM_IOT_CONFIG_H
