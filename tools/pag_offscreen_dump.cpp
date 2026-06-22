// Created by vompom on 2026/06/21.
//
// @Description
//   pag_offscreen_dump：Stage 4.1 PoC CLI。
//
//   作用：
//     1) 加载一个 .pag 文件；
//     2) 用 pag_sdk::Engine 离屏渲染指定 progress（默认 0.5，即动画中点）；
//     3) 把得到的 RGBA8888（Premultiplied） dump 成 24/32-bit BMP，
//        方便在树莓派 / macOS 上直接双击预览，验证 libpag 渲染上下文确实
//        在目标平台跑通。
//
//   为什么选 BMP：
//     - 不引入 libpng / libjpeg 依赖，零额外构建成本；
//     - 32-bit BGRA top-down BMP 在 Linux/macOS/Win 默认看图器都能开；
//     - Stage 4.2 起 gstpagfilter 自己做 alpha blend，与 BMP 无关，
//       这个工具只为 Stage 4.1 验收保留。
//
//   用法：
//     pag_offscreen_dump <pag_path> [progress=0.0~1.0] [out=out.bmp]
//                       [width=pag_width] [height=pag_height]
//
//   退出码：
//     0 成功；非 0 任一步骤失败（path 错 / libpag 关 / Surface 失败 / 写盘失败）。
//
//   编译开关：本可执行体仅在 VM_IOT_ENABLE_LIBPAG=ON 时挂入构建，
//   见根 CMakeLists.txt。

#include "pag_sdk.h"
#include "log.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

/* BMP 文件头与信息头按 1 字节对齐——BMP 标准。 */
#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t bfType      = 0x4D42;  // 'BM'
    uint32_t bfSize      = 0;
    uint16_t bfReserved1 = 0;
    uint16_t bfReserved2 = 0;
    uint32_t bfOffBits   = 54;       // sizeof(file)+sizeof(info)
};
struct BmpInfoHeader {
    uint32_t biSize          = 40;
    int32_t  biWidth         = 0;
    int32_t  biHeight        = 0;    // 负值表示 top-down
    uint16_t biPlanes        = 1;
    uint16_t biBitCount      = 32;
    uint32_t biCompression   = 0;    // BI_RGB
    uint32_t biSizeImage     = 0;
    int32_t  biXPelsPerMeter = 2835;
    int32_t  biYPelsPerMeter = 2835;
    uint32_t biClrUsed       = 0;
    uint32_t biClrImportant  = 0;
};
#pragma pack(pop)
static_assert(sizeof(BmpFileHeader) == 14, "BmpFileHeader must be 14 bytes");
static_assert(sizeof(BmpInfoHeader) == 40, "BmpInfoHeader must be 40 bytes");

/* RGBA(premul) → BGRA(straight，仅做通道交换 + 反 premul) 的最小转换。
 *
 * BMP 的 32 位格式按惯例是 BGRA-straight。这里我们做两件事：
 *   1) R↔B 交换（RGBA→BGRA）；
 *   2) 反预乘：c = c_pm / a  （a==0 时整像素归零）。
 *
 * 反预乘的目的纯粹是「预览看着接近原图」——本工具仅供 Stage 4.1 验收，
 * 不进入渲染管线。gstpagfilter 自己的 blend 会直接吃 premultiplied RGBA，
 * 不走这个路径。 */
void rgba_premul_to_bgra_straight(const uint8_t* src, uint8_t* dst,
                                  size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t r = src[4 * i + 0];
        uint8_t g = src[4 * i + 1];
        uint8_t b = src[4 * i + 2];
        uint8_t a = src[4 * i + 3];
        if (a == 0) {
            dst[4 * i + 0] = 0;
            dst[4 * i + 1] = 0;
            dst[4 * i + 2] = 0;
            dst[4 * i + 3] = 0;
        } else {
            /* 反 premul，clamp 到 [0,255]。整数运算避免引入浮点。 */
            auto un = [a](uint8_t c) -> uint8_t {
                int v = (c * 255 + a / 2) / a;
                return static_cast<uint8_t>(v > 255 ? 255 : v);
            };
            dst[4 * i + 0] = un(b);
            dst[4 * i + 1] = un(g);
            dst[4 * i + 2] = un(r);
            dst[4 * i + 3] = a;
        }
    }
}

bool write_bmp_bgra_top_down(const std::string& path,
                             int width, int height,
                             const std::vector<uint8_t>& bgra) {
    const size_t row_bytes = static_cast<size_t>(width) * 4u;
    if (bgra.size() != row_bytes * static_cast<size_t>(height)) {
        LOGE("write_bmp: buffer size mismatch, got={} expected={}",
             bgra.size(), row_bytes * static_cast<size_t>(height));
        return false;
    }
    BmpFileHeader fh;
    BmpInfoHeader ih;
    fh.bfSize        = 54u + static_cast<uint32_t>(bgra.size());
    ih.biWidth       = width;
    ih.biHeight      = -height;          // top-down，避免行翻转
    ih.biSizeImage   = static_cast<uint32_t>(bgra.size());

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        LOGE("write_bmp: cannot open '{}' for write", path);
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    ofs.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
    ofs.write(reinterpret_cast<const char*>(bgra.data()),
              static_cast<std::streamsize>(bgra.size()));
    if (!ofs.good()) {
        LOGE("write_bmp: write failed for '{}'", path);
        return false;
    }
    return true;
}

int parse_int(const char* s, int fallback) {
    if (!s || !*s) return fallback;
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}
double parse_double(const char* s, double fallback) {
    if (!s || !*s) return fallback;
    try {
        return std::stod(s);
    } catch (...) {
        return fallback;
    }
}

void usage(const char* argv0) {
    fprintf(stderr,
        "Usage: %s <pag_path> [progress=0.5] [out=out.bmp] [width] [height]\n"
        "  progress : 0.0~1.0，对应 PAG 时间轴位置；默认 0.5\n"
        "  out      : 输出 BMP 路径；默认 out.bmp\n"
        "  width    : 离屏 Surface 宽；不填则用 PAG 原始宽\n"
        "  height   : 离屏 Surface 高；不填则用 PAG 原始高\n"
        "Example:\n"
        "  %s pag/PAG_LOGO.pag 0.5 logo.bmp 320 240\n",
        argv0, argv0);
}

}  // namespace

int main(int argc, char** argv) {
    /* 与 src/main.cpp 行为一致：headless Pi 环境强制 surfaceless EGL 平台，
     * 避免 Mesa 因 DISPLAY 空而走 x11 后端导致 eglInitialize 失败。
     * 第三参 0 = 已设置则不覆盖，留运维手动覆盖口子。 */
    ::setenv("EGL_PLATFORM", "surfaceless", 0);

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const std::string pag_path = argv[1];
    const double      progress = (argc >= 3) ? parse_double(argv[2], 0.5) : 0.5;
    const std::string out_path = (argc >= 4) ? argv[3] : "out.bmp";
    int               width    = (argc >= 5) ? parse_int(argv[4], 0) : 0;
    int               height   = (argc >= 6) ? parse_int(argv[5], 0) : 0;

    setup_logger("info");

    if (!pag_sdk::is_enabled()) {
        LOGE("pag_offscreen_dump: libpag is disabled at compile time, abort");
        return 3;
    }
    LOGI("pag_offscreen_dump: sdk={}, file='{}', progress={:.3f}, out='{}'",
         pag_sdk::sdk_version(), pag_path, progress, out_path);

    /* 第一次 Make 时如果 width/height 还未给，先用一个临时小尺寸
     * Surface 把 PAGFile 加载起来，拿到 PAG 原始尺寸后再重建。
     * 这样做是因为 PAGFile::Load 的元信息接口 width/height 不依赖
     * Surface，但我们的 Engine::Make 把 Surface 创建合并在一起，
     * 没有「先 Load 再决定」的中间状态。Stage 4.1 的工具阶段这点
     * 浪费可接受；gstpagfilter 真接入时 Surface 尺寸来自 caps，
     * 不会有这个问题。 */
    if (width <= 0 || height <= 0) {
        auto probe = pag_sdk::Engine::Make(pag_path, 16, 16);
        if (!probe) {
            LOGE("pag_offscreen_dump: probe Engine::Make failed");
            return 4;
        }
        if (width  <= 0) width  = probe->pag_width();
        if (height <= 0) height = probe->pag_height();
        LOGI("pag_offscreen_dump: probed pag size = {}x{}, "
             "using surface = {}x{}",
             probe->pag_width(), probe->pag_height(), width, height);
    }

    auto eng = pag_sdk::Engine::Make(pag_path, width, height);
    if (!eng) {
        LOGE("pag_offscreen_dump: Engine::Make({}x{}) failed", width, height);
        return 5;
    }
    LOGI("pag_offscreen_dump: engine ready, duration={}us",
         static_cast<long long>(eng->duration_us()));

    const size_t row_bytes = static_cast<size_t>(width) * 4u;
    std::vector<uint8_t> rgba(row_bytes * static_cast<size_t>(height));
    if (!eng->render_frame_rgba(progress, rgba.data(), row_bytes)) {
        LOGE("pag_offscreen_dump: render_frame_rgba failed");
        return 6;
    }
    LOGI("pag_offscreen_dump: rendered {}x{} RGBA premul ok", width, height);

    std::vector<uint8_t> bgra(rgba.size());
    rgba_premul_to_bgra_straight(rgba.data(), bgra.data(),
                                 static_cast<size_t>(width) *
                                 static_cast<size_t>(height));

    if (!write_bmp_bgra_top_down(out_path, width, height, bgra)) {
        LOGE("pag_offscreen_dump: write_bmp_bgra_top_down failed");
        return 7;
    }
    LOGI("pag_offscreen_dump: wrote '{}' ({} bytes)",
         out_path, static_cast<long long>(bgra.size() + 54));
    return 0;
}
