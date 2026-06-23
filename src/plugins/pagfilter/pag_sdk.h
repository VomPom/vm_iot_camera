//
// Created by vompom on 2026/06/19.
//
// @Description
//   对 Tencent/libpag 的薄抽象层。
//
//   设计动机：
//     1) 让 gstpagfilter.cpp 完全不依赖 libpag 的真实头文件——避免主工程的
//        编译单元（含单测）每次都吃一份 libpag 的庞大头依赖；
//     2) 通过 CMake 选项 VM_IOT_ENABLE_LIBPAG 在编译期切换「真集成 / stub」：
//          - ON  ：pag_sdk.cpp 调真 libpag（pag/pag.h、libpag::pag）；
//          - OFF ：pag_sdk.cpp 仅返回固定字符串与 nullptr 状态，主二进制
//                  不引入 libpag 依赖；
//     3) Stage 4.1 起新增 pag_sdk::Engine，把 PAGFile / PAGSurface / PAGPlayer
//        三件套封到 pimpl 后面，外面只看 RGBA 像素接口。gstpagfilter 永远
//        看不见 libpag 类型。
//
//   线程模型：
//     - is_enabled / sdk_version / selftest_load / Engine::Make：主/控制线程；
//     - Engine::renderFrameRGBA：**streaming 线程**调用（与构造同线程），
//       libpag 的 PAGPlayer 不是线程安全，调用方必须保证 Engine 对象在
//       同一线程构造与使用。
//

#ifndef VM_IOT_PAG_SDK_H
#define VM_IOT_PAG_SDK_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pag_sdk {

/* libpag 是否在编译期被真正链接进来。 */
bool is_enabled();

/* 返回 PAG SDK 版本字符串。
 *   - enabled  : libpag 的 pag::PAG::SDKVersion()；
 *   - disabled : 固定串 "libpag(disabled)"。 */
std::string sdk_version();

/* 自检：尝试加载 .pag 文件并打印宽 / 高 / 时长。
 * Stage 3 引入；Stage 4 起 gstpagfilter 不再直接调它，统一走 Engine。
 * 但 main.cpp 启动期保留这条路径作为冒烟检查。 */
bool selftest_load(const std::string& pag_file_path);

/* ─────────────────────── Stage 4.1：Engine 抽象 ───────────────────────
 * 一个 Engine 封装一组「PAGFile + PAGSurface + PAGPlayer」，离屏渲染到
 * RGBA8888（Premultiplied）。生命周期与所在 GstElement 实例 1:1 绑定。
 *
 * 为什么是 pimpl：libpag 的类型（pag::PAGFile / pag::PAGSurface / pag::PAGPlayer）
 * 是带 EGL/GLES 依赖的庞大类，绝不能在 pag_sdk.h 出现。我们用前向声明的
 * Impl 把所有 libpag 类型藏到 .cpp 里，对外只暴露 POD 输入输出。
 *
 * 关闭分支（VM_IOT_ENABLE_LIBPAG=OFF）：Make() 永远返回 nullptr，从而
 * 让调用方代码（gstpagfilter / tools/pag_offscreen_dump）走 passthrough/退化
 * 路径而无需 #ifdef 分支。 */
class Engine {
public:
    /* 工厂：加载 pag_file_path，按 width×height 分配离屏 Surface。
     * 失败原因（文件不存在 / 解析失败 / Surface 创建失败 / libpag 关闭）
     * 走日志，统一返回 nullptr。永远不抛异常。 */
    static std::unique_ptr<Engine> Make(const std::string& pag_file_path,
                                        int width,
                                        int height);

    ~Engine();

    /* 返回 PAGFile 原始尺寸（来自 .pag 元信息），不是 Surface 尺寸。
     * Stage 4.3 里 gstpagfilter 用它判断是否需要 PAGScaleMode。 */
    int  pag_width() const;
    int  pag_height() const;

    /* PAG 动画总时长（微秒）。0 表示静态 / 不可推进。
     * gstpagfilter 用 buffer PTS 模 duration_us 算 progress。 */
    int64_t duration_us() const;

    /* Surface 实际尺寸（构造时传入的 width/height）。 */
    int  surface_width() const;
    int  surface_height() const;

    /* 推进时间轴到 progress01 ∈ [0,1]，并把当前帧渲染到 dst_rgba_premul。
     * dst_rgba_premul 必须至少有 row_bytes × surface_height() 字节，
     * row_bytes 必须 >= surface_width() × 4。
     *
     * 像素格式：固定 RGBA_8888 + Premultiplied alpha（libpag 默认输出）。
     *
     * 返回 true 表示渲染并 readPixels 成功；false 表示底层 flush /
     * readPixels 失败，调用方应直接走 passthrough。 */
    bool render_frame_rgba(double progress01,
                           void*  dst_rgba_premul,
                           size_t row_bytes);

    /* ──────────────────── Stage 5：图层替换 API ────────────────────
     * 所有 replace_* 仅修改 PAGFile 内部图层引用，对下一次 render_frame_rgba
     * 生效；调用方仍要负责按业务节奏推 progress。
     *
     * 线程模型：与 render_frame_rgba 同一调用线程（streaming 线程）。
     * 跨线程调用的同步由更上层（pagfilter）负责。 */

    /* 当前 PAG 文件中的可编辑文本图层数量。
     * stub 分支永远返回 0；真分支返回 PAGFile::numTexts()。 */
    int num_texts() const;

    /* 当前 PAG 文件中的图像占位图层数量（image placeholder）。
     * stub 分支永远返回 0；真分支返回 PAGFile::numImages()。 */
    int num_images() const;

    /* 把第 idx 个文本图层替换为给定 UTF-8 字符串。
     * 越界 / stub 分支 / libpag 内部失败时返回 false（仅 LOGW 不抛）。
     * 仅修改 text 字段，字体/字号/颜色保留 PAG 内既有设置。 */
    bool replace_text(int idx, const std::string& utf8);

    /* 把第 idx 个图像占位图层替换为给定 RGBA8888 像素帧。
     * rgba_data 必须按 row_bytes × height 字节布局，row_bytes >= width*4。
     * alpha 语义：opaque（不参与 PAG 内 alpha 混合，PAG 自己按图层 mask 处理）。
     *
     * 实现说明：内部用 PAGImage::FromPixels 复制一份像素到 libpag 管理的
     * 内存，调用返回后 rgba_data 可立即被复用/释放。**频繁替换**会触发
     * libpag 内部纹理重建，调用方应自行限频（pagfilter 已有
     * pag-replace-image-every 节流）。
     *
     * 越界 / 参数非法 / stub 分支 / 内部失败时返回 false。 */
    bool replace_image_from_rgba(int           idx,
                                 const void*   rgba_data,
                                 int           width,
                                 int           height,
                                 size_t        row_bytes);

private:
    Engine();  /* 禁止外部直接 new，统一走 Make */
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pag_sdk

#endif  // VM_IOT_PAG_SDK_H
