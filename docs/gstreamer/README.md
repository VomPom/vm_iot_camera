# GStreamer Element 速查（vm_iot 项目）

> 本目录整理 vm_iot 项目当前实际使用到的所有 GStreamer Element。
> 所有 Element 清单来源：`cmake-build-utm/cmd.log` 在 UTM aarch64 上跑通后导出的 pipeline。

## 当前 pipeline 拓扑

```text
[source]
  v4l2src  ─►  image/jpeg, 1280x720@60
              └─► jpegparse ─► jpegdec
                  └─► videoconvert ─► videoscale ─► videorate
                      └─► video/x-raw, I420, 1280x720@30
                          └─► videoconvert
                              └─► glupload ─► glcolorconvert
                                  └─► glshader (name=f0)
                                      └─► glcolorconvert ─► gldownload
                                          └─► videoconvert
                                              └─► tee  name=t        # raw 锚点
                                                   ├──► [branch:snapshot]
                                                   │     queue ─► valve(snap_valve)
                                                   │       └─► videoconvert
                                                   │           └─► jpegenc
                                                   │               └─► multifilesink(snap_sink)
                                                   │
                                                   └──► (主线编码段)
                                                         queue ─► videoconvert ─► x264enc ─► h264parse
                                                           └─► tee  name=enc_t   # 码流锚点
                                                                ├──► [branch:main]
                                                                │     queue ─► rtph264pay (pay0)
                                                                │
                                                                └──► [branch:record]
                                                                      queue(no-leaky) ─► valve(rec_valve)
                                                                        └─► identity(rec_tail)        # 稳定锚点
                                                                              ┊
                                                                  [Record 模块运行期动态 link]
                                                                              ┊
                                                                              ▼
                                                                        mp4mux ─► filesink           # 每段一个独立子 bin
```

## 分类目录

### 1. Source（视频源）
- [v4l2src](./v4l2src.md) —— Linux V4L2 摄像头采集源

### 2. Parser / Decoder（解析与解码）
- [jpegparse](./jpegparse.md) —— MJPEG 流帧边界对齐与容错
- [jpegdec](./jpegdec.md) —— JPEG → 原始像素解码
- [h264parse](./h264parse.md) —— H.264 NAL 重组、SPS/PPS 注入

### 3. Filter / Converter（CPU 通用变换）
- [videoconvert](./videoconvert.md) —— 像素格式互转
- [videoscale](./videoscale.md) —— 分辨率缩放
- [videorate](./videorate.md) —— 帧率重采样（丢帧 / 重复帧）

### 4. OpenGL（GPU 滤镜段）
- [glupload](./glupload.md) —— 系统内存 → GL 纹理上传
- [glcolorconvert](./glcolorconvert.md) —— GL 内的色彩空间转换
- [glshader](./glshader.md) —— 自定义片元着色器滤镜
- [gldownload](./gldownload.md) —— GL 纹理 → 系统内存回读

### 5. Generic（流控 / 分流 / 缓冲 / 占位）
- [tee](./tee.md) —— 单输入多分支零拷贝复制
- [queue](./queue.md) —— 缓冲与解耦线程边界
- [valve](./valve.md) —— 数据闸阀（按需开闭）
- [identity](./identity.md) —— 透传占位元素（项目录像副线锚点 `rec_tail`）

### 6. Encoder（编码器）
- [x264enc](./x264enc.md) —— H.264 软编（libx264）
- [jpegenc](./jpegenc.md) —— 单帧 JPEG 软编

### 7. Payloader / Muxer / Sink（封装与落盘）
- [rtph264pay](./rtph264pay.md) —— H.264 → RTP 打包（RFC 6184）
- [multifilesink](./multifilesink.md) —— 多文件序列输出
- [mp4mux](./mp4mux.md) —— ISO BMFF / mp4 容器封装（项目录像副线核心，每段一个独立实例）
- [filesink](./filesink.md) —— 单文件落盘（与 mp4mux 配对每段一个）

### 附：已不在主线使用
- [splitmuxsink](./splitmuxsink.md) —— 容器边界自动切片。早期录像副线方案，
  因 split-now 语义"切完旧段立刻开新段"无法优雅 stop，已替换为
  `identity 锚点 + 动态 mp4mux+filesink 子 bin`（业内方案 1）。文档保留作为历史参考。

## 阅读建议

- 想理解整条流为什么这样接：先看本页拓扑，再按出现顺序逐个翻 Element。
- 想调优某个环节：直接跳转到对应 Element，重点看「内部工作原理与数据流程」「性能开销」两节。
- 每篇 Element 文档统一结构：基本信息 / Pad 与 Caps / 关键属性 / 项目内用法 / 内部原理 / 性能与坑。

## 备注

- 备选但当前 pipeline 未启用的编码后端（`openh264enc` / `x265enc` / `h265parse` / `rtph265pay`）
  暂未单独建档，如启用后写入 `cmd.log`，再补对应文档。
- 各 Element 的「项目内用法」段落直接引用 [`pipeline_builder.cpp`](../../src/pipeline/pipeline_builder.cpp)
  里的真实拼装片段，方便对照源码。
