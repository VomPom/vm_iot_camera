//
// Created by vompom on 2026/06/19.
//
// @Description
//   pag_sdk 的实现。通过预处理宏 VM_IOT_ENABLE_LIBPAG 决定调真 libpag 还是 stub。
//
//   两个分支的实现都需要：
//     - 完全无异常逃逸（GStreamer 流水线最忌讳 C++ 异常穿过 C 边界）；
//     - 不在 streaming 线程被构造，但 Engine::render_frame_rgba 必须能在
//       streaming 线程被反复调用；
//     - 失败时仅返回 false / nullptr，由调用方决定如何打印（避免重复打印）。
//
//   Stage 4.1 起 Engine（pimpl）在此实现：
//     - 真分支：持有 pag::PAGFile / pag::PAGSurface / pag::PAGPlayer 三件套；
//     - stub 分支：Impl 不存在，Engine::Make 永远返回 nullptr。
//

#include "pag_sdk.h"

#include "log.h"

#include <fstream>
#include <string>

#if VM_IOT_ENABLE_LIBPAG
/* 真集成分支：vendored 的 libpag 头。
 * 注意：libpag 公共头位于 include/pag/pag.h，types/AlphaType/ColorType 也都在这里。 */
#include <pag/pag.h>
#endif

namespace pag_sdk {

bool is_enabled() {
#if VM_IOT_ENABLE_LIBPAG
    return true;
#else
    return false;
#endif
}

std::string sdk_version() {
#if VM_IOT_ENABLE_LIBPAG
    return std::string("libpag/") + pag::PAG::SDKVersion();
#else
    return "libpag(disabled)";
#endif
}

bool selftest_load(const std::string& pag_file_path) {
#if VM_IOT_ENABLE_LIBPAG
    if (pag_file_path.empty()) {
        LOGW("pag_sdk: selftest skipped, file path is empty");
        return false;
    }
    /* 先做存在性检查，避免 libpag 内部 fopen 失败的难读栈。 */
    std::ifstream f(pag_file_path, std::ios::binary);
    if (!f.good()) {
        LOGW("pag_sdk: selftest open failed, path='{}'", pag_file_path);
        return false;
    }
    f.close();

    auto file = pag::PAGFile::Load(pag_file_path);
    if (!file) {
        LOGW("pag_sdk: selftest PAGFile::Load returned null, path='{}'",
             pag_file_path);
        return false;
    }

    LOGI("pag_sdk: selftest loaded '{}', size={}x{}, duration={}us, "
         "numTexts={}, numImages={}",
         pag_file_path,
         file->width(), file->height(),
         static_cast<long long>(file->duration()),
         file->numTexts(), file->numImages());
    return true;
#else
    (void)pag_file_path;
    LOGI("pag_sdk: selftest skipped (libpag disabled at compile time), "
         "would have loaded '{}'", pag_file_path);
    return false;
#endif
}

/* ─────────────────────── Engine 实现 ─────────────────────── */

#if VM_IOT_ENABLE_LIBPAG

/* Impl 仅在真分支存在；stub 分支不定义 Impl，Engine::Make 直接返回 nullptr，
 * 但 Engine 的 ~Engine / 构造/接口符号仍然要有定义（avoid unresolved），
 * 见后面 stub 分支里的空壳实现。 */
struct Engine::Impl {
    std::shared_ptr<pag::PAGFile>    file;
    std::shared_ptr<pag::PAGSurface> surface;
    /* PAGPlayer 走值类型——按官方示例，PAGPlayer 在栈/堆上由我们持有，
     * Surface 与 Composition 通过 setSurface / setComposition 注入。 */
    pag::PAGPlayer                   player;
    int                              surface_w = 0;
    int                              surface_h = 0;
};

std::unique_ptr<Engine> Engine::Make(const std::string& pag_file_path,
                                     int width,
                                     int height) {
    if (pag_file_path.empty()) {
        LOGW("pag_sdk::Engine::Make: empty pag_file_path");
        return nullptr;
    }
    if (width <= 0 || height <= 0) {
        LOGW("pag_sdk::Engine::Make: invalid surface size {}x{}", width, height);
        return nullptr;
    }
    /* 存在性预检——同 selftest_load 的考虑：避免 libpag 内部失败栈。 */
    {
        std::ifstream f(pag_file_path, std::ios::binary);
        if (!f.good()) {
            LOGW("pag_sdk::Engine::Make: open failed '{}'", pag_file_path);
            return nullptr;
        }
    }

    auto file = pag::PAGFile::Load(pag_file_path);
    if (!file) {
        LOGW("pag_sdk::Engine::Make: PAGFile::Load returned null '{}'",
             pag_file_path);
        return nullptr;
    }

    auto surface = pag::PAGSurface::MakeOffscreen(width, height);
    if (!surface) {
        LOGW("pag_sdk::Engine::Make: PAGSurface::MakeOffscreen({}x{}) failed",
             width, height);
        return nullptr;
    }

    /* unique_ptr 的构造在私有 ctor 上：用 new 而非 make_unique，
     * 因为 make_unique 在类外，无法访问 private ctor。 */
    std::unique_ptr<Engine> eng(new Engine());
    eng->impl_              = std::make_unique<Impl>();
    eng->impl_->file        = std::move(file);
    eng->impl_->surface     = std::move(surface);
    eng->impl_->surface_w   = width;
    eng->impl_->surface_h   = height;
    eng->impl_->player.setSurface(eng->impl_->surface);
    eng->impl_->player.setComposition(eng->impl_->file);
    /* autoClear=true 是默认；显式声明意图：每帧先清透明背景，由调用方
     * 的 alpha blending 负责跟底图叠合，不允许残留上一帧。 */
    eng->impl_->player.setAutoClear(true);

    LOGI("pag_sdk::Engine::Make ok: pag='{}' pag_size={}x{} "
         "surface={}x{} duration={}us",
         pag_file_path,
         eng->impl_->file->width(), eng->impl_->file->height(),
         width, height,
         static_cast<long long>(eng->impl_->file->duration()));
    return eng;
}

Engine::Engine()  = default;
Engine::~Engine() = default;

int Engine::pag_width()  const { return impl_->file->width();  }
int Engine::pag_height() const { return impl_->file->height(); }

int64_t Engine::duration_us() const {
    return impl_->file->duration();
}

int Engine::surface_width()  const { return impl_->surface_w; }
int Engine::surface_height() const { return impl_->surface_h; }

bool Engine::render_frame_rgba(double progress01,
                               void*  dst_rgba_premul,
                               size_t row_bytes) {
    if (!dst_rgba_premul) {
        LOGW("pag_sdk::Engine::render: dst is null");
        return false;
    }
    const size_t min_row = static_cast<size_t>(impl_->surface_w) * 4u;
    if (row_bytes < min_row) {
        LOGW("pag_sdk::Engine::render: row_bytes={} < min_row={}",
             row_bytes, min_row);
        return false;
    }
    /* clamp 而不是拒绝：streaming 线程拿到的 PTS 可能略超 duration，
     * 让 PAG 自动循环到下一周期。 */
    if (progress01 < 0.0) progress01 = 0.0;
    if (progress01 > 1.0) progress01 = progress01 - static_cast<int>(progress01);

    impl_->player.setProgress(progress01);
    if (!impl_->player.flush()) {
        /* flush 失败常见原因：EGL context 创建失败（headless 树莓派无
         * X / wayland 时遇到）。仅 LOGW，不刷屏。 */
        LOGW("pag_sdk::Engine::render: PAGPlayer::flush failed");
        return false;
    }
    if (!impl_->surface->readPixels(pag::ColorType::RGBA_8888,
                                    pag::AlphaType::Premultiplied,
                                    dst_rgba_premul,
                                    row_bytes)) {
        LOGW("pag_sdk::Engine::render: PAGSurface::readPixels failed");
        return false;
    }
    return true;
}

/* ─────────────────── Stage 5：图层替换实现（真分支） ─────────────────── */

int Engine::num_texts() const {
    if (!impl_ || !impl_->file) return 0;
    return impl_->file->numTexts();
}

int Engine::num_images() const {
    if (!impl_ || !impl_->file) return 0;
    return impl_->file->numImages();
}

bool Engine::replace_text(int idx, const std::string& utf8) {
    if (!impl_ || !impl_->file) return false;
    const int n = impl_->file->numTexts();
    if (idx < 0 || idx >= n) {
        LOGW("pag_sdk::Engine::replace_text: idx={} out of range [0,{})", idx, n);
        return false;
    }
    /* getTextData 返回 std::shared_ptr<TextDocument>；按官方示例只改 text
     * 字段，其它字体/字号/颜色保留素材原设。NULL 表示该图层不可编辑
     * （理论上 idx 在 [0,numTexts) 内不会返 NULL，但保留兜底）。 */
    auto td = impl_->file->getTextData(idx);
    if (!td) {
        LOGW("pag_sdk::Engine::replace_text: getTextData({}) returned null", idx);
        return false;
    }
    td->text = utf8;
    impl_->file->replaceText(idx, td);
    return true;
}

bool Engine::replace_image_from_rgba(int          idx,
                                     const void*  rgba_data,
                                     int          width,
                                     int          height,
                                     size_t       row_bytes) {
    if (!impl_ || !impl_->file) return false;
    if (!rgba_data || width <= 0 || height <= 0) {
        LOGW("pag_sdk::Engine::replace_image_from_rgba: invalid pixel desc "
             "({}x{}, data={})",
             width, height, rgba_data ? "ok" : "null");
        return false;
    }
    if (row_bytes < static_cast<size_t>(width) * 4u) {
        LOGW("pag_sdk::Engine::replace_image_from_rgba: row_bytes={} < {}",
             row_bytes, static_cast<size_t>(width) * 4u);
        return false;
    }
    const int n = impl_->file->numImages();
    if (idx < 0 || idx >= n) {
        LOGW("pag_sdk::Engine::replace_image_from_rgba: idx={} out of range [0,{})",
             idx, n);
        return false;
    }
    /* PAGImage::FromPixels 会复制一份像素，调用返回后 rgba_data 可立即重用。
     * AlphaType 选 Opaque：摄像头帧没有 alpha 概念，PAG 内部按图层 mask
     * 处理透明，不要在这里多此一举传 Premultiplied。 */
    auto img = pag::PAGImage::FromPixels(
        static_cast<const uint8_t*>(rgba_data),
        width, height,
        static_cast<size_t>(row_bytes),
        pag::ColorType::RGBA_8888,
        pag::AlphaType::Opaque);
    if (!img) {
        LOGW("pag_sdk::Engine::replace_image_from_rgba: FromPixels failed");
        return false;
    }
    impl_->file->replaceImage(idx, img);
    return true;
}

#else  /* VM_IOT_ENABLE_LIBPAG == 0 ─ stub 分支：Engine 永远造不出来 */

/* Impl 仍然要有定义，否则 unique_ptr<Impl> 析构在 .cpp 里无法实例化。
 * stub 分支不持有任何状态，仅作占位。 */
struct Engine::Impl {};

std::unique_ptr<Engine> Engine::Make(const std::string& pag_file_path,
                                     int width,
                                     int height) {
    (void)pag_file_path;
    (void)width;
    (void)height;
    LOGI("pag_sdk::Engine::Make: libpag disabled at compile time, "
         "returning nullptr");
    return nullptr;
}

Engine::Engine()  = default;
Engine::~Engine() = default;

int     Engine::pag_width()       const { return 0; }
int     Engine::pag_height()      const { return 0; }
int64_t Engine::duration_us()     const { return 0; }
int     Engine::surface_width()   const { return 0; }
int     Engine::surface_height()  const { return 0; }

bool Engine::render_frame_rgba(double, void*, size_t) {
    /* stub 分支永远造不出 Engine 对象——这里被调用说明逻辑出错，
     * 但仍保持「不抛、返回 false」的契约。 */
    return false;
}

/* Stage 5 stub：Engine 在 stub 分支根本不会被构造，但保留符号定义
 * 避免单测/上层在编译期 reference 缺失。 */
int  Engine::num_texts()  const { return 0; }
int  Engine::num_images() const { return 0; }
bool Engine::replace_text(int, const std::string&) { return false; }
bool Engine::replace_image_from_rgba(int, const void*, int, int, size_t) {
    return false;
}

#endif

}  // namespace pag_sdk
