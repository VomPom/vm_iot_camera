# vm_iot 代码风格与工程规范

> 适用范围：仅对 **vm_iot 仓库** 生效。
> 本规则解决一件事——让任何模型 / 协作者写出的代码，无论新增还是修改，
> 在风格、注释、错误处理、资源管理、构建集成上，与现有代码**逐字逐句一致**。
> 与 `Element.md` 是平行规则，互不重叠（那份只管 GStreamer Element 文档同步）。

---

## 0. 总原则

- **优先编辑现有文件，不主动创建新文件**；新增文件必须有职责必要性。
- **不主动写 README 或 *.md 文档**，除非用户明确要求或属于 `Element.md` 规则覆盖的范围。
- **不擅自重构 / 调整代码顺序**，除非任务就是重构。
- 所有注释、日志文案、错误信息一律 **中文 + 半角符号**，技术术语保留英文（如 `pipeline`、`caps`、`ref`、`probe`、`unprepared`）。
- 注释解释 **"为什么这样做"**，不要复述代码本身在做什么。

---

## 1. 语言与构建

- 标准：**C++17**（`CMakeLists.txt` 已锁定，禁止使用 C++20 特性）。
- 编译选项：默认带 `-Wall -Wextra -Wpedantic`，新增代码必须 0 warning 通过。
- 头文件 include 风格：**扁平**，所有 `#include "xxx.h"` **不带子目录前缀**
  - ✅ `#include "log.h"` `#include "branch_base.h"`
  - ❌ `#include "common/log.h"` `#include "branches/branch_base.h"`
  - 新增 `src/` 下子目录时，必须同步加到 `CMakeLists.txt` 的 `target_include_directories(vm_iot PUBLIC ...)` 列表。
- 源文件通过 `GLOB_RECURSE` 自动收集，新增 `.cpp` 通常无需改 CMake；但**新增独立可执行体**（如 `tools/`、`cli/`）必须显式 `add_executable` 并解释为什么独立。

---

## 2. 头文件骨架

每个新头文件必须严格遵循以下骨架，逐行对齐：

```cpp
//
// Created by vompom on YYYY/MM/DD.
//
// @Description
//   一段（必要时多段）说明：本文件干什么、生命周期约定、关键纪律、
//   作用域（明确不做的事）。
//

#ifndef VM_IOT_<UPPER_SNAKE_NAME>_H
#define VM_IOT_<UPPER_SNAKE_NAME>_H

#pragma once   // 与 include guard 双保险，参考 v4l2_prober.h / log.h

#include <gst/gst.h>            // 第三方/系统头优先
#include <gst/rtsp-server/rtsp-server.h>

#include <mutex>                // STL 头次之
#include <string>
#include <vector>

#include "config.h"             // 项目内头最后

class XxxDeps;                  // 必要时前置声明，减少头依赖

class Xxx {
public:
    // ...
};

#endif //VM_IOT_<UPPER_SNAKE_NAME>_H
```

硬性要求：

- 文件头注释格式 **逐字模仿** `branch_base.h` / `v4l2_prober.h`；
  「@Description」段落必须填充实质内容，不允许像 `log.h` 那样空着（除非真的是
  10 行以内的 trivial 头）。
- include guard 命名：`VM_IOT_<文件名转大写蛇形>_H`，下划线分词。
- include 顺序：**第三方/系统 → STL → 项目内**，每段空一行。
- 公共类用 `class`；纯数据聚合用 `struct`（如 `RecordConfig`、`Capability`）。

---

## 3. 实现文件骨架

```cpp
//
// Created by vompom on YYYY/MM/DD.
//
// @Description
//   见 xxx.h 顶部说明。      ← 实现文件不重复写，只指向头文件
//

#include "xxx.h"
#include "log.h"

#include <filesystem>          // 仅本 .cpp 用到的额外头
```

实现侧惯例：

- 命名空间别名只在 `.cpp` 内用：`namespace fs = std::filesystem;`，**不要写到头**。
- **不写 `using namespace std;`**。STL 类型一律带 `std::` 前缀。

---

## 4. 命名

| 实体 | 风格 | 示例 |
|---|---|---|
| 类 | `UpperCamelCase` | `BranchBase` `RtspServer` `ShaderFilter` |
| 函数 / 方法 | `lower_snake_case` | `attach_to_media` `client_count` `make_location_template` |
| 成员变量 | `lower_snake_case_` 末尾下划线 | `pipeline_` `mu_` `auto_timeout_id_` `cfg_` |
| 静态回调 | `s_on_xxx` | `s_on_unprepared` |
| 持锁内部函数 | `xxx_locked_` 后缀 | `detach_locked_` `set_valve_drop_locked` |
| 常量 / 枚举值 | `UpperCamel` 或 `kCamel`，看上下文 | 与现有文件保持一致 |
| 文件名 | `lower_snake_case.{h,cpp}` | `branch_base.h` `v4l2_prober.cpp` |
| 命名空间 | `lower_snake_case` | `v4l2_prober` |
| Pipeline 内 GstElement 名 | `<branch>_<role>` | `rec_valve` `rec_sink` `snap_sink` |

---

## 5. 注释

注释分两种风格，**按用途严格区分**：

### 5.1 多行说明性注释 → `/* ... */`

放在函数声明上方、非平凡代码块之上。允许跨多行，**用于解释意图、纪律、坑**。

```cpp
/* 顺序很关键：先让子类停掉所有"还在用元素"的异步活动，
 * 再 unref 元素本身。子类可能在 on_detaching_locked 里访问 element()。 */
if (pipeline_ || !elements_.empty()) {
    on_detaching_locked();
}
```

### 5.2 文档注释 → `///` 或 `/** ... */`

用于公共 API 的 Doxygen 风格描述、`@param` / `@return`。参考
`v4l2_prober.h` 的 `Capability` 与 `probe()`。

### 5.3 行内简短注释 → `//`

用于一两个词的标注，例如：

```cpp
GstElement* pipeline_ = nullptr;                          // 持 ref
std::unordered_map<std::string, GstElement*> elements_;   // 名字→元素，各持 ref
```

### 5.4 禁止

- 禁止 `// TODO` 之外的纯流水账（"set the valve drop to true"）。
- 禁止保留注释掉的死代码——直接删。
- 禁止英文标点和中文标点混用；中文段落用半角逗号、句号也接受，但要全段一致。

---

## 6. 日志

- 仅使用 `log.h` 提供的宏：`LOGT/LOGD/LOGI/LOGW/LOGE`，**禁止**直接 `spdlog::info(...)` 或 `printf`/`std::cout`。
- 格式串使用 fmt 风格 `{}`，不要用 `%s`/`%d`。
- **每条日志带一个模块前缀**，冒号后空格，全小写：
  - ✅ `LOGI("record: start (manual)");`
  - ✅ `LOGW("v4l2_prober: open({}) failed: {}", device, std::strerror(errno));`
  - ❌ `LOGI("Started recording");`
- 级别选择：
  - `LOGE`：进程级失败、必须人工介入；
  - `LOGW`：本次操作失败但服务可继续（如 attach 失败回滚、设备探测失败回退 hardcode caps）；
  - `LOGI`：生命周期里程碑（attach / start / stop / detach）；
  - `LOGD/LOGT`：调试详情，生产默认关闭。
- **禁止**在已知失败路径用 `LOGE` 制造误报——典型例子：`probe()` 失败用 `LOGW` 而非 `LOGE`，因为上层有兜底。

---

## 7. 错误处理

- **不抛异常**。这是项目硬约定（grep 全仓库无 `throw`）。失败一律走以下三条之一：
  1. 返回 `bool` + 出参 `std::string& err`（短错误码字符串，如 `"pipeline_not_ready"` `"invalid_duration"`），方便控制通道直接回传给客户端；
  2. 返回 optional / 空容器 / `nullptr`，调用方有兜底逻辑；
  3. 启动期不可恢复错误：在 `main.cpp` 打印**人类可读的多行框式提示**并 `return <非零码>`，参考现有摄像头检测错误信息。
- 错误码字符串风格：`lower_snake_case`，不带空格、不带句号。
- 资源失败要清理：参考 `BranchBase::attach_to_media` 的"全部抓到才生效，否则全部回滚"模式。

---

## 8. 资源 / 生命周期

GStreamer / GLib 这套 C 库混 C++ 时极易出错，本仓库形成了固定模式，**禁止偏离**：

- 持有 `GstElement*` / `GstRTSPMedia*` 等 ref-counted 对象的成员变量，必须在注释里标 `// 持 ref` 或 `// 不拥有，仅持指针`，**二选一，不要含糊**。
- 释放路径只走一条函数（如 `detach_locked_`），多个入口（析构、`shutdown`、信号回调）都进它，保证幂等。
- 对 `gpointer user` 的回调入口，统一这套模板：
  ```cpp
  static void s_on_xxx(GstSomething* /*s*/, gpointer user) {
      auto* self = static_cast<MyClass*>(user);
      if (!self) return;
      std::lock_guard<std::mutex> lk(self->mu_);
      // ...
  }
  ```
- 不在持锁函数里调用可能反向加锁的 GStreamer API（比如 `g_signal_connect` 在锁外做，参考 `branch_base.cpp` 第 69-71 行的注释）。

---

## 9. 并发

- 互斥锁名称统一 `mu_`，类型统一 `std::mutex`，加锁统一 `std::lock_guard<std::mutex> lk(mu_);`。
- 「持锁版本」函数加 `_locked_` 后缀，**调用方必须已持锁**，函数内**不可再加锁**。
  在函数文档注释中显式说明前置条件，例：
  > /* 调用方（BranchBase::attach_to_media）已持 mu_。 */
- GLib timeout / pad probe 这类异步活动，停止动作必须在元素 `unref` 之前完成
  （走 `on_detaching_locked()`），参考 `Record::on_detaching_locked` 中的 `g_source_remove`。

---

## 10. Branch 子模块（src/branches/）

新增挂在 RTSP media pipeline tee/enc_tee 后面的副线（如 `detect/`、`cloud_upload/`）必须：

1. 继承 `BranchBase`，目录结构 `src/branches/<name>/<name>.{h,cpp}`；
2. 只覆写四个虚函数：`branch_name()` / `required_elements()` / `on_attached_locked()` / `on_detaching_locked()`，**不允许**自管 pipeline ref；
3. Pipeline 中插入的元素命名前缀必须用 branch 名缩写（`rec_*` / `snap_*` / `det_*`），方便 `gst_bin_get_by_name`；
4. 在 `main.cpp` 通过 `branches.push_back(&xxx)` 注册，禁止改 `RtspServer` 类；
5. 业务 API 失败统一用「`bool` 返回 + `std::string& err`」三段式（参考 `Record::start/stop_recording/auto_record`）；
6. 实现 `format_status(std::string& out)`，输出 `key=value\n` 行式文本，键名前缀 = branch 名。

---

## 11. 配置

- 全部从 `config/default.yaml` 读取，解析层在 `src/app/config.{h,cpp}`。
- 新增配置项必须：
  1. 在 `config.h` 加字段 + 默认值；
  2. 在 `config.cpp` 加 yaml 解析 + 类型校验 + 默认兜底；
  3. 在 `default.yaml` 加示例并写中文注释解释取值范围；
  4. 启动期 `LOGI` 把生效值打出来（参考 `record: configured dir=... segment=...`）。
- 不允许从环境变量直接读配置，统一走 yaml。

---

## 12. 测试

- 单测放 `tests/`，文件名 `test_<被测模块>.cpp`，注册到 `tests/CMakeLists.txt`。
- 使用 GoogleTest（CMake 已通过 `FetchContent` 拉取）。
- **跨平台单测**必须能在 macOS（无 `/dev/video*`）下编译并通过——`v4l2_prober.{h,cpp}` 的非 Linux stub 是模板，新加 Linux-only 模块时同样要做。
- 测试不连真实摄像头、不起 RTSP 服务，只测纯函数 / 状态机 / 解析逻辑。

---

## 13. 改动检查清单（提交前）

写完任何代码改动后，用这份清单自检一遍：

- [ ] include 顺序：第三方 → STL → 项目内，每段空行隔开
- [ ] 所有 `#include "xxx.h"` 都是扁平路径，没有子目录
- [ ] 新文件头注释完整（`Created by vompom` + `@Description` 实质内容）
- [ ] 类的成员变量带末尾下划线，且对 ref 持有/借用做了注释标注
- [ ] 所有日志带模块前缀，级别选用合理
- [ ] 失败路径走 `bool + std::string& err` 或回滚式资源管理，无 `throw`
- [ ] 多线程入口加锁，`_locked_` 后缀函数前置条件已注释
- [ ] 新增 GstElement 已按 `Element.md` 补 `docs/gstreamer/<element>.md`
- [ ] `-Wall -Wextra -Wpedantic` 0 warning 通过
- [ ] 不为本次需求之外的代码做"顺手优化"

---

_最后更新：2026-06-15_
