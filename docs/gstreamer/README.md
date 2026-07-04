# GStreamer Element 速查（vm_iot 项目）

> 本目录整理 vm_iot 项目当前实际使用到的所有 GStreamer Element。
> 所有 Element 清单来源：`cmake-build-utm/cmd.log` 在 UTM aarch64 上跑通后导出的 pipeline。

## 当前 pipeline 拓扑

```text
[source]
  v4l2src  ─►  image/jpeg, 1280x720@60   (或 raw caps，由 V4L2Prober + CapsRanker 选出)
              └─► jpegparse ─► jpegdec     (仅 jpeg 路径)
                  └─► videoconvert ─► videoscale ─► videorate
                      └─► video/x-raw, I420, 1280x720@30   (下游统一 caps，cfg.capture.pixfmt)
                          └─► videoconvert
                              └─► [GL filter 段可选] glupload ─► glcolorconvert
                                  └─► glshader (name=f0)
                                      └─► glcolorconvert ─► gldownload
                                          └─► videoconvert
                                              └─► [pagfilter (name=pag0)，可选]
                                                # cfg.filter.pag.enabled=true 时插入；pag-file 为空时 passthrough，
                                                # 非空则按 .pag 渲染并 alpha-blend 到 I420，支持 PLAYING 热切
                                                # (cfg.filter.pag.* + iotcamctl pag *)
                                                  └─► tee  name=t        # raw 锥点
                                                       ├──► [branch:snapshot]
                                                       │     queue (leaky=downstream, max-buffers=2, silent=true)
                                                       │       └─► valve(snap_valve, drop=true)
                                                       │           └─► videoconvert ─► jpegenc
                                                       │               └─► multifilesink(snap_sink, post-messages=true)
                                                       │
                                                       ├──► [branch:face]   ✅ 可选（cfg.face.enabled=true 时接入）
                                                       │     queue (leaky=downstream, max-buffers=2, silent=true)
                                                       │       └─► valve(face_valve)
                                                       │           └─► videorate ─► videoconvert(RGB)
                                                       │               └─► facedetect(name=face0, display=false)
                                                       │                   └─► appsink(face_appsink)        # 检测结果走 pipeline bus
                                                       │
                                                       ├──► [branch:face_preview]   ⏳ 可选（preview_jpeg.enabled=true 时额外挂上）
                                                       │     queue ─► videorate ─► videoconvert(RGB)
                                                       │       └─► facedetect(display=true)
                                                       │           └─► videoconvert ─► jpegenc
                                                       │               └─► valve(face_prev_valve, drop=true)
                                                       │                   └─► appsink(face_jpeg_sink)
                                                       │
                                                       └──► (主线编码段)
                                                             queue (leaky=downstream, max-buffers=2)
                                                               └─► videoconvert ─► video/x-raw,format=I420
                                                                   └─► x264enc / openh264enc / x265enc
                                                                       └─► h264parse | h265parse (config-interval=1)
                                                                           └─► tee  name=enc_t   # 码流锥点
                                                                                ├──► [branch:main]
                                                                                │     queue (leaky=downstream, max-buffers=2)
                                                                                │       └─► rtph264pay | rtph265pay
                                                                                │           name=pay0 pt=96 mtu=1400
                                                                                │
                                                                                └──► [branch:record]   ⏳ 规划中
                                                                                      # append_branch_record() 当前为空实现，
                                                                                      # 所以 launch 串里不会出现录像副线。
                                                                                      # 待恢复时重新接 mp4mux + filesink 子 bin。
```

拓扑说明：

- **锥点二选一**：要“像素”去 `tee name=t`，要“码流”去 `tee name=enc_t`。
  新增副线严禁再起第三个 tee。
- **副线首件必须是 queue**：snapshot / detect / motion 这类“可丢”副线用
  `leaky=downstream`；record 这类“不可丢”副线要用 no-leaky 大缓冲。
- **valve 默认 drop=true**：副线首端黑顺关闭，由 `ControlChannel`
  要用时才打开。snapshot 才会出现“单帧 jpeg”的效果。
- **编码后才走 tee**：main RTP 与未来的 record 副线共用同一份
  H.264 / H.265 ES，录像不需要重新编码。

码流锥点还计划了两条未启用的副线（代码里只预留插槽）：

| 副线       | 锥点    | 落点            | 状态与 queue 策略                       |
|------------|---------|-----------------|---------------------------------------|
| main(rtp)  | enc_t.  | rtph26Xpay      | 已实现 / leaky=downstream(2)             |
| snapshot   | t.      | jpegenc + file  | 已实现 / leaky=downstream(2)             |
| face       | t.      | facedetect + appsink | 已实现 / leaky=downstream(2)；cfg.face.enabled=true |
| face_preview | t.    | facedetect(display=true) + jpegenc + appsink | 可选 / leaky=downstream(2)；preview_jpeg.enabled=true |
| record     | enc_t.  | mp4mux + file   | 规划中 / 预计 no-leaky 大缓冲          |
| motion     | t.      | msg / event     | 规划中 / leaky=downstream(2)             |

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
- [identity](./identity.md) —— 透传占位元素（项目录像副线锰点 `rec_tail`）
- [pagfilter](./pagfilter.md) —— 自研滤镜（libpag 渲染 + 文本/视频图层 PLAYING 热替换）

### 6. Computer Vision（计算机视觉）
- [facedetect](./facedetect.md) —— OpenCV Haar 人脸检测（gst-plugins-bad opencv）
- [videorate](./videorate.md) —— 视频帧率适配器（face 副线把 30 fps 降到 5 fps）

### 7. Encoder（编码器）
- [x264enc](./x264enc.md) —— H.264 软编（libx264）
- [jpegenc](./jpegenc.md) —— 单帧 JPEG 软编

### 8. Payloader / Muxer / Sink（封装与落盘）
- [rtph264pay](./rtph264pay.md) —— H.264 → RTP 打包（RFC 6184）
- [multifilesink](./multifilesink.md) —— 多文件序列输出
- [mp4mux](./mp4mux.md) —— ISO BMFF / mp4 容器封装（项目录像副线核心，每段一个独立实例）
- [filesink](./filesink.md) —— 单文件落盘（与 mp4mux 配对每段一个）
- [appsink](./appsink.md) —— 应用层接收点（face 副线终结点，未来接 HTTP 端点）

### 附：已不在主线使用
- [splitmuxsink](./splitmuxsink.md) —— 容器边界自动切片。早期录像副线方案，
  因 split-now 语义“切完旧段立刻开新段”无法优雅 stop，已替换为
  `identity 锥点 + 动态 mp4mux+filesink 子 bin`。录像副线本身当前也未启用
  （见上方拓扑 *⏳ 规划中* 标记），文档作为历史参考保留。

## 阅读建议

- 想理解整条流为什么这样接：先看本页拓扑，再按出现顺序逐个翻 Element。
- 想调优某个环节：直接跳转到对应 Element，重点看「内部工作原理与数据流程」「性能开销」两节。
- 每篇 Element 文档统一结构：基本信息 / Pad 与 Caps / 关键属性 / 项目内用法 / 内部原理 / 性能与坑。

## 备注

- 备选但当前 pipeline 未启用的编码后端（`openh264enc` / `x265enc` / `h265parse` / `rtph265pay`）
  暂未单独建档，如启用后写入 `cmd.log`，再补对应文档。
- 各 Element 的「项目内用法」段落直接引用 [`pipeline_builder.cpp`](../../src/pipeline/pipeline_builder.cpp)
  里的真实拼装片段，方便对照源码。
