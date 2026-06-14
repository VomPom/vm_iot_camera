# GStreamer Element 文档维护规则

> 适用范围：仅对 **vm_iot 仓库** 生效。
> 本规则只解决一件事——确保 pipeline 中每个被使用的 GStreamer Element 在 `docs/gstreamer/` 下都有一份对应的、结构一致的解释文档。

---

## 1. 触发条件

在以下任一文件中，新增了**此前在本仓库中从未使用过**的 GStreamer Element（例如首次引入 `vaapih264enc`、`nvv4l2decoder`、`glimagesink` 等）：

- `src/pipeline/pipeline_builder.cpp`
- `src/branches/**/*.cpp`（如 `snapshot.cpp`）
- `src/rtsp/rtsp_server.cpp`
- `src/filter/shader_filter.cpp`
- `scripts/gst_launch/*.sh` 中长期保留的 pipeline 模板
- 其它任何会进入主分支的 GStreamer pipeline 构建代码

判定"是否首次使用"的方法：在仓库范围内 `grep` 该 Element 名称，若除本次改动外没有其它命中，即视为首次引入。

---

## 2. 强制动作

一次改动中必须**同时**完成以下两件事，**不允许只改代码不补文档**。

### 2.1 新建 Element 解释文档

- **路径**：`docs/gstreamer/<element_name>.md`
- **命名**：文件名 = Element 名，全小写，与 `gst-inspect-1.0` 输出的名称完全一致。
- **结构**：必须包含且仅包含以下三大节，顺序固定：

  1. **基本信息**
     - 所属分类：Source / Filter / Convert / Encoder / Decoder / Parser / Mux / Demux / Pay / Depay / Sink / GL / Tee / Queue / Valve / …
     - 所属插件包：Ubuntu apt 包名 + `gst-inspect-1.0` 中显示的 plugin 名
     - Pad 能力：sink/src caps 关键字段（如 `video/x-raw`、`video/x-raw(memory:GLMemory)`、`image/jpeg`、`video/x-h264`）
     - 关键属性：列出常用 property，给出默认值与含义
     - 使用举例：一行可直接跑通的 `gst-launch-1.0` 命令
     - **项目内用法**：指明在 `pipeline_builder.cpp` 等本仓库文件中的拼接位置、上游 / 下游 Element

  2. **内部工作原理与数据流程**
     - 必须用 mermaid 图（`flowchart LR` 或 `sequenceDiagram`）画出数据流 / 状态机 / 关键回调
     - 文字补充：caps negotiation 行为、是否拷贝内存、是否阻塞、线程模型

  3. **性能开销与其他补充**
     - CPU / GPU / 内存拷贝开销的定性评估
     - 常见坑：caps 不匹配、property 顺序、与 `queue` / `videoconvert` 配合等
     - 调试技巧：`GST_DEBUG=<element>:5`、`gst-inspect-1.0 <element>` 中要重点看的字段

### 2.2 更新目录索引

- **路径**：`docs/gstreamer/README.md`
- **动作**：在 Element 所属分类小节下追加一行跳转链接：
  `- [<element_name>](./<element_name>.md) — 一句话说明`
- 若该分类在 README 中尚不存在，需新增对应的标题分类，再把链接挂入。

---

## 3. 兜底约束

- 在生成 PR / patch 时，如果检测到第 1 节的触发条件却**未**附带 `docs/gstreamer/<element>.md` 与 `docs/gstreamer/README.md` 的同步改动，必须**主动提示并补齐**，不要等用户提醒。
- 仅用于一次性临时验证、不进入主分支的 Element，可在 commit message 中显式标注 `[no-doc: throwaway]` 以跳过本规则。

---

_最后更新：2026-06-14_
