//
// Created by vompom on 2026/06/19.
//
// @Description
//   pag_sdk 的实现。通过预处理宏 VM_IOT_ENABLE_LIBPAG 决定调真 libpag 还是 stub。
//
//   两个分支的实现都需要：
//     - 完全无异常逃逸（GStreamer 流水线最忌讳 C++ 异常穿过 C 边界）；
//     - 不在 streaming 线程被调用；
//     - 失败时仅返回 false / nullptr，由调用方决定如何打印（避免重复打印）。
//

#include "pag_sdk.h"

#include "log.h"

#include <fstream>
#include <string>

#if VM_IOT_ENABLE_LIBPAG
/* 真集成分支：vendored 的 libpag 头。
 * 注意：libpag 公共头位于 include/pag/pag.h（取决于 libpag 的版本），
 * 不同 release 的目录布局有差异；这里走最常见的形态。Stage 4 如发现
 * 需要更细粒度（PAGFile / PAGPlayer / PAGSurface），再单独 include。 */
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
    /* libpag 的版本字符串接口随版本演进过；优先 PAG::SDKVersion()，
     * 老版本若不可用，编译期会立刻报错，到时再降级。 */
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

    /* PAGFile::Load 失败时返回空 shared_ptr；不抛异常。
     * Stage 3 仅打印元信息后立刻销毁；Stage 4 才会持有用于渲染。 */
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
    /* 关闭分支：不读盘、不调任何 libpag 接口；仅日志说明已禁用。
     * 显式 (void) 让 stub 分支即使在 -Werror=unused-parameter 严格档下也不报错，
     * 不依赖项目顶层的 -Wno-unused-parameter 编译选项。 */
    (void)pag_file_path;
    LOGI("pag_sdk: selftest skipped (libpag disabled at compile time), "
         "would have loaded '{}'", pag_file_path);
    return false;
#endif
}

}  // namespace pag_sdk
