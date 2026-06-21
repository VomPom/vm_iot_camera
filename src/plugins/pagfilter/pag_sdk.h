//
// Created by vompom on 2026/06/19.
//
// @Description
//   对 Tencent/libpag 的薄抽象层（Stage 3 引入）。
//
//   设计动机：
//     1) 让 gstpagfilter.cpp 完全不依赖 libpag 的真实头文件——避免主工程的
//        编译单元（含单测）每次都吃一份 libpag 的庞大头依赖；
//     2) 通过 CMake 选项 VM_IOT_ENABLE_LIBPAG 在编译期切换「真集成 / stub」：
//          - ON  ：pag_sdk.cpp 调真 libpag（pag/pag.h、libpag::pag）；
//          - OFF ：pag_sdk.cpp 仅返回固定字符串与 nullptr 状态，主二进制
//                  不引入 libpag 依赖；
//        这是 Stage 3 与原计划的显式偏离——避免在主分支默认引入慢编译依赖；
//     3) Stage 4 起把渲染、PAGFile 句柄、setProgress / flush 都收敛到这层，
//        gstpagfilter 永远只看 PagSdk::Engine 抽象。
//
//   线程模型：本头里所有函数都是「主/控制线程」语义——启动期注入版本日志、
//   按需做 selftest 加载，**绝不在 streaming 线程调用**。Stage 4 起渲染相关
//   的入口会单独标注，并由 PagSdk::Engine 自己保证 `PAGPlayer` 不跨线程使用。
//

#ifndef VM_IOT_PAG_SDK_H
#define VM_IOT_PAG_SDK_H

#include <string>

namespace pag_sdk {

/* libpag 是否在编译期被真正链接进来。
 *   - true  : VM_IOT_ENABLE_LIBPAG=ON 时编译，pag_sdk 调真 libpag；
 *   - false : OFF 时编译，pag_sdk 仅是 stub。
 * 暴露这个 const bool 主要给 selftest 日志判断走哪个分支。 */
bool is_enabled();

/* 返回 PAG SDK 版本字符串。
 *   - enabled  : libpag 的 pag::PAG::SDKVersion()；
 *   - disabled : 固定串 "libpag(disabled)"。
 * 不会抛异常；仅做最小调用，无 IO，启动期一次即可。 */
std::string sdk_version();

/* 自检：尝试加载 .pag 文件并打印宽 / 高 / 时长。
 * Stage 3 的唯一「真链接 libpag」入口；用来证明：
 *   - libpag 头能 include；
 *   - 符号可链接；
 *   - 进程启动期调用一次不崩。
 * 返回值：
 *   - true  : 加载成功（仅 enabled 分支可能为 true）；
 *   - false : 关闭 / 文件不存在 / 解析失败；细节走日志，不抛异常。
 * Stage 3 不做渲染，加载完立刻销毁。 */
bool selftest_load(const std::string& pag_file_path);

}  // namespace pag_sdk

#endif  // VM_IOT_PAG_SDK_H
