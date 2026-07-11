# vm_iot_camera

[English](README.md) · **简体中文**

一个轻量的 RTSP 摄像头守护进程。它从 V4L2 设备拉帧,经过一段可选的
GLSL 后处理 libpag 特效滤镜,用纯软件 H.264 / H.265 后端编码,最后通过
`gst-rtsp-server` 把流暴露出去，Web 平台可视化控制，配套第二个二
进制 `vm_iot_ctl` 通过一对 FIFO 与正在运行的守护进程通信,可以在不
重启流的前提下切换特效、抓帧、查询状态等逻辑。运行期支持 UVC 摄像头
热拔插自愈,daemon 不重启、页面不刷新,画面 1–3s 内自动恢复。

项目目前在 **Raspberry Pi 5(8 GB)** + **Ubuntu 24.04** 上开发与验
证,USB UVC 摄像头从前置 USB 口接入。RTSP 守护进程、控制 FIFO、
Web 控制台全部跑在这一台设备上。

<p align="left">
  <img src=".imgs/device.jpg" alt="vm_iot 参考硬件:Raspberry Pi 5 8GB + USB 摄像头,Ubuntu 24.04" width="35%"/>
</p>

---

## Demo

下面几张截图与录屏摘自实机运行,涵盖桌面/移动端 Web 控制台、实时画
面回放、控制台交互与人脸检测叠加。

<table>
  <tr>
    <td align="center" width="50%">
      <b>Web 控制台 — 桌面端</b><br/>
      <img src=".imgs/all.jpg" alt="桌面端 Web 控制台总览" width="100%"/>
    </td>
    <td align="center" width="50%">
      <b>Web 控制台 — 移动端</b><br/>
      <img src=".imgs/mobile.png" alt="移动端访问 Web 控制台" width="60%"/>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <b>实时画面回放</b><br/>
      <a href=".imgs/video1.mp4" title="点击播放 video1.mp4">
        <img src=".imgs/video1.gif" alt="RTSP 实时画面预览(点击查看 MP4)" width="100%"/>
      </a><br/>
      <sub>点击 GIF 打开高质量 MP4。</sub>
    </td>
    <td align="center" width="50%">
      <b>控制台交互</b><br/>
      <a href=".imgs/video2.mp4" title="点击播放 video2.mp4">
        <img src=".imgs/video2.gif" alt="Web 控制台交互录屏(点击查看 MP4)" width="100%"/>
      </a><br/>
      <sub>点击 GIF 打开高质量 MP4。</sub>
    </td>
  </tr>
  <tr>
    <td align="center" colspan="2">
      <b>人脸检测 Demo</b><br/>
      <a href=".imgs/face.mp4" title="点击播放 face.mp4">
        <img src=".imgs/face.gif" alt="实时人脸检测叠加框(点击查看 MP4)" width="60%"/>
      </a><br/>
      <sub>实时人脸检测 + 检测框叠加。点击 GIF 打开高质量 MP4。</sub>
    </td>
  </tr>
</table>

---

## 特性

- **YAML 配置 + CLI 覆盖。** 优先级:`CLI > YAML > 内置默认值`。
- **自动 V4L2 能力协商。** 启动期 `V4L2Prober` 列出摄像头真实支持
  的能力,`CapsRanker` 选出最接近请求 `(fmt, w, h, fps)` 的组合
  (USB 摄像头默认走 `prefer_jpeg`)。
- **三种纯软编后端:** `x264`、`openh264`(H.264)与 `x265`(H.265)。
  硬件后端(VAAPI / NVENC / V4L2 M2M)未集成,因为目标部署环境
  (aarch64 上 UTM 里的 Ubuntu)没有可用的硬件编码器。
- **GL shader 滤镜,运行期热切。** 单一文件 `effects.frag` 容纳所
  有变体(passthrough / mosaic / invert / ...),`filter_type`
  在运行期决定走哪个分支。切换时着色器**程序不重新编译**,因此画
  面不会卡顿。运行期还可以 `reload` 从磁盘重新加载 shader。
- **libpag 素材热叠加。** 自研 `pagfilter` element 可以把 `.pag`
  素材(贴纸/字幕/动画)在 `PLAYING` 状态下**热切素材文件、替换
  文本图层、开画中画(把当前视频帧塞进 PAG 图层)、调节画中画上
  行帧率**;构建期通过 `-DVM_IOT_ENABLE_LIBPAG=ON` 决定是否真的
  链接 libpag(关掉时相关配置解析仍生效,运行期跳过该段,方便 CI
  与最小化构建)。
- **基于 FIFO 的实时控制。** 守护进程侧的 `ControlChannel` 监听
  一个命名管道,`vm_iot_ctl` 是配套客户端。命令包括 `filter`、
  `reload`、`status`、`snapshot`、`pag`、`face`、`raw` 等。
- **抓帧。** 编码器旁边挂着一个 `tee` + `valve` 分支;`snapshot`
  命令会打开 valve 一帧的时间,把当帧写成 JPEG。
- **UVC 热拔插自愈。** 运行期拔插 USB 摄像头 daemon 不重启、页面不刷新,
  画面停顿 1–3s 后自动恢复;详见
  [docs/reference/hotplug_recovery.md](docs/reference/hotplug_recovery.md)。
- **人脸检测副线。** 从原始像素锥点接一条独立分支,跑 `gst-plugins-bad`
  的 OpenCV Haar `facedetect`,默认 5 fps 节流。主 RTSP 线路完全零侵入
  (`display=false`,不改一个像素);检测坐标通过 pipeline bus 上报,
  `FaceBranch` 汇聚后写入 events FIFO(NDJSON `kind:"faces"`),Web 控
  制台通过 WebSocket 订阅并在 `<video>` 上叠 canvas 画框。运行期用
  `vm_iot_ctl face on/off` 开关。
- **一体化 Web 控制台。** [`web/`](web/) 下 Fastify + 单文件 ESM
  前端,把 daemon 完整功能面搬进浏览器:
  - **多协议画面预览** —— WebRTC(通过 mediamtx WHEP,低延迟)/
    HLS(兼容性兜底)/ 裸 RTSP URL 三档一键切换;
  - **人脸框实时叠加** —— canvas 层跟着 `<video>` 缩放,可一键关闭;
  - **控制面板** —— filter 切换/上下轮/`reload`、snapshot、pag
    (set-file/set-text/set-replace-image/throttle)、record
    (rec / stop / auto N 秒;后端副线仍在规划中,面板 UI 已就绪)、
    raw 命令输入框;
  - **实时状态** —— 顶栏与状态卡通过 `/ws/events` 订阅 daemon 1 Hz
    status 广播,自动刷新 uptime / 客户端数 / 码率 / 编码器 / 分辨率
    / 当前 filter / PAG 素材;
  - **快捷键** —— `1-9` 切 filter、`Space` 抓帧、`R` 录制开关、
    `/` 聚焦 raw 输入框;
  - **响应式布局** —— 桌面双列、移动端(≤1024px)单列纵向堆叠。
- **所有 RTSP 客户端共用同一条 pipeline**
  (`gst_rtsp_media_factory_set_shared(TRUE)`),CPU 不会随观看人
  数线性增长。
- **优雅退出**:`SIGINT` / `SIGTERM` 由 `GMainLoop` 统一处理。
- **日志**:走 `spdlog`。
- **单元测试**:GoogleTest,由 CMake 用 `FetchContent` 自动拉取,
  不需要系统级安装。
- 构建脚本会把 `assets/` 软链到可执行文件旁边,因此 `config/`、
  `pag/`、`shaders/` 这些运行期资源跟二进制是同一个视图。改完文件
  重启即可生效,**不需要重新编译**。

---

## 仓库结构

```
vm_iot/
├── CMakeLists.txt              # 顶层构建脚本
├── assets/                     # 运行期资源(单一软链目标)
├── docs/
├── scripts/
│   ├── gst_launch/             # 独立 gst-launch 辅助脚本(probe / preview / record / rtsp)
│   └── bench/                  # 编码器基准测试(如 h265_compare.sh)
├── src/
│   ├── main.cpp                # 入口:初始化、解析参数、主循环
│   ├── app/                    # YAML 加载 + CLI 合并 + 信号处理
│   ├── common/                 # 日志 + pipeline 字符串美化打印
│   ├── pipeline/               # PipelineBuilder:拼接完整 gst-launch 字符串
│   ├── filter/                 # ShaderFilter:可热切的 glshader 包装
│   ├── branches/
│   │   └── snapshot/           # 抓帧分支(tee + valve + jpegenc)
│   │   └── pag/                # PAG分支
│   ├── control/                # ControlChannel:基于 FIFO 的命令服务
│   ├── rtsp/                   # gst-rtsp-server 包装
│   ├── util/                   # V4L2Prober、CapsRanker
│   └── cli/
│       └── iotcamctl.cpp       # vm_iot_ctl 源码(只翻译协议,不含业务逻辑)
└── plugins/
    └── pagfilter/              # 自研 GstElement,把 .pag 渲染结果叠到主线 I420 上
```

---

## 依赖

- 带可用 V4L2 设备(`/dev/videoN`)的 Linux
- CMake ≥ 3.22
- 支持 C++17 的编译器(GCC 或 Clang)
- GStreamer 1.0,需要 RTSP server、GL 以及 base/good/bad/ugly 插件
- `yaml-cpp`
- `spdlog`
- 首次构建需要联网,以便 CMake 通过 `FetchContent` 拉取 GoogleTest。

### Debian / Ubuntu 一键安装

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake pkg-config git \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstrtspserver-1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad  gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav        gstreamer1.0-tools \
    gstreamer1.0-gl \
    libyaml-cpp-dev libspdlog-dev
```

---

## 构建

```bash
git clone <this-repo> vm_iot
cd vm_iot
cmake -S . -B build
cmake --build build -j
```

构建产物有三个二进制:

| 二进制       | 源码                                | 用途                                                      |
|--------------|-------------------------------------|-----------------------------------------------------------|
| `vm_iot`     | `src/`(除 `src/cli` 之外的全部)   | RTSP 守护进程。                                           |
| `vm_iot_ctl` | `src/cli/iotcamctl.cpp`             | 轻客户端。通过 FIFO 与守护进程通信。                      |
| `probe_dev`  | `tests/tools/probe_dev.cpp`         | 独立的 V4L2 能力 dump 工具,常用来跟 `v4l2-ctl --list-formats-ext` 对账。 |

构建完成后:

```
build/
├── vm_iot
├── vm_iot_ctl
├── probe_dev
└── assets -> ../assets      # 单条软链就覆盖了 config/ pag/ shaders/
```

`make install`(或 CPack 的安装步骤)会把 `vm_iot` 与 `vm_iot_ctl`
拷到 `bin/`。`probe_dev` 是调试辅助工具,不会被安装。

跳过单元测试:

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
```

---

## 运行

```bash
./build/vm_iot                                # 默认读 assets/config/default.yaml
./build/vm_iot -c assets/config/default.yaml  # 显式指定配置文件
./build/vm_iot --port 8555 --bitrate 6000     # CLI 覆盖
```

服务端会监听:

```
rtsp://<host>:8554/live
```

快速回放:

```bash
# H.264(后端 x264 / openh264)
gst-launch-1.0 -v rtspsrc location=rtsp://127.0.0.1:8554/live ! \
    rtph264depay ! avdec_h264 ! videoconvert ! autovideosink

# H.265(后端 x265)
gst-launch-1.0 -v rtspsrc location=rtsp://127.0.0.1:8554/live ! \
    rtph265depay ! avdec_h265 ! videoconvert ! autovideosink

# 编码无关(decodebin 自动识别):
ffplay rtsp://127.0.0.1:8554/live
```

---

## Pipeline 拓扑

守护进程在启动期拼出一条 `gst-launch` 风格的字符串交给
`gst-rtsp-server`。下面这张拓扑跟 [src/pipeline/pipeline_builder.cpp](src/pipeline/pipeline_builder.cpp)
里 `PipelineBuilder::build` 生成的字符串一一对应。整个 pipeline
只会出现两个 tee 锥点：一个含“原始像素”(`tee name=t`)，一个含
“编码后码流”(`tee name=enc_t`)，所有副线必须二选一贴着走。

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
                                                       ├──► [branch:face]         # cfg.face.enabled=true 时才存在
                                                       │     queue (leaky=downstream, max-buffers=2, silent=true)
                                                       │       └─► valve(face_valve, drop=<!enabled_at_start>)
                                                       │           └─► videorate ─► video/x-raw,framerate=<fps_limit>/1
                                                       │               └─► videoconvert ─► video/x-raw,format=RGB
                                                       │                   └─► facedetect(name=face0, display=false,
                                                       │                                  profile=<cascade>, min-size-*=<min_size_px>,
                                                       │                                  scale-factor=<scale_factor>,
                                                       │                                  min-neighbors=<min_neighbors>)
                                                       │                       └─► fakesink(face_appsink, async=false, sync=false, silent=true)
                                                       │                       # 坐标走 GST_MESSAGE_ELEMENT('facedetect')
                                                       │                       # → FaceBranch → events FIFO(NDJSON kind:"faces") → Web canvas 叠框
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
                                                                                      # launch 串里不会出现录像副线。
                                                                                      # 为未来重接 mp4mux + filesink 子 bin 预留位置。
```

拓扑说明：

- **锥点二选一。** 要“像素”去 `tee name=t`，要“码流”去
  `tee name=enc_t`。新增副线严禁再起第三个 tee。
- **副线首件必须是 queue。** snapshot / detect / motion 这类“可丢”副线
  用 `leaky=downstream`，不会反压主线；record 这类“不可丢”副线要用
  no-leaky 大缓冲。
- **valve 默认 drop=true。** 可选副线首端默认关闭，`ControlChannel`
  要用时才打开。snapshot 就是这么“一命令一 jpeg”的。
- **先编码再 tee。** 编码器被提到了“码流 tee”之前，未来的 record 副线
  可以零成本复用主线 RTP 同一份 H.264 / H.265 ES。

副线总表（已实现 + 规划中）：

| 副线       | 锥点      | 落点              | 状态       | queue 策略              |
|------------|---------|-----------------|------------|--------------------------|
| main(rtp)  | enc_t.  | rtph26Xpay      | 已实现     | leaky=downstream(2)      |
| snapshot   | t.      | jpegenc + file  | 已实现     | leaky=downstream(2)      |
| face       | t.      | facedetect + fakesink（bus msg → events FIFO） | 已实现 | leaky=downstream(2) |
| record     | enc_t.  | mp4mux + file   | 规划中     | non-leaky、大缓冲          |
| motion     | t.      | msg / event     | 规划中     | leaky=downstream(2)      |

拓扑里出现的每个 element 在 [docs/gstreamer/](docs/gstreamer/README.md)
下都有一页专项说明。

---

## 编码后端

只支持纯软件编码器。目标部署(aarch64 + UTM + Ubuntu)上没有可用的
硬件编码器,所以 VAAPI / NVENC / V4L2 M2M 后端被剥离了。

| backend    | 编码  | GStreamer 元素    | 说明                                                            |
|------------|-------|-------------------|-----------------------------------------------------------------|
| `x264`     | H.264 | `x264enc`         | 默认。兼容性最好,速度/质量折中合理。                            |
| `openh264` | H.264 | `openh264enc`     | Cisco 的 H.264 编码器。不支持 B 帧,`bframes` 被忽略。           |
| `x265`     | H.265 | `x265enc`         | 同等画质码率更低,CPU 约 1.5–2×;客户端必须支持 HEVC。           |

`PipelineBuilder::build`([src/pipeline/pipeline_builder.cpp](src/pipeline/pipeline_builder.cpp))
会按后端选择对应的 parser 和 RTP payloader:H.264 后端用
`h264parse` + `rtph264pay`,`x265` 用 `h265parse` + `rtph265pay`。
摄像头协商出来的像素格式由 `capture.pixfmt` 决定,经 `videoconvert`
统一转成三种软编都接受的 `I420`。`filter.enabled=true` 时,会在采
集与编码之间插入一段 GL(`glupload ! glcolorconvert ! glshader !
gldownload`)。

---

## 实时控制:`vm_iot_ctl`

`vm_iot_ctl` 是一个独立的 CLI 客户端。它本身**不含任何业务逻辑**:
把友好的子命令翻译成一行 `ControlChannel` 协议字符串,写入请求
FIFO,从应答 FIFO 读取回执,并把结果映射成进程退出码。冷启动 < 100
ms,运行期只依赖 libc 与 libstdc++。

```bash
# 切 shader 特效
./build/vm_iot_ctl filter 2          # filter_type = 2
./build/vm_iot_ctl filter next       # 切到下一个变体
./build/vm_iot_ctl filter prev
./build/vm_iot_ctl filter get        # 查询当前变体

# 重新从磁盘加载 shader 文件(无需重启守护进程)
./build/vm_iot_ctl reload

# 打印运行期状态(运行时长、客户端数、编码器、滤镜……)
./build/vm_iot_ctl status

# 单次 JPEG 抓帧
./build/vm_iot_ctl snapshot                       # 由守护进程决定路径
./build/vm_iot_ctl snapshot /tmp/frame.jpg        # 显式路径

# 直接发送原始协议行(调试用)
./build/vm_iot_ctl raw "filter set 1"

# 人脸检测副线(控 face_valve不重启 pipeline)
./build/vm_iot_ctl face on           # 打开 face_valve，开始检测
./build/vm_iot_ctl face off          # 关闭 face_valve，暂停检测
./build/vm_iot_ctl face get          # 查询当前开关状态（on / off）
```

### 人脸检测

`face.enabled: true`（[assets/config/default.yaml](assets/config/default.yaml)
里默认已开）时，守护进程会在 raw 锥点 `tee name=t` 下多拉一条
副线，跑 OpenCV Haar `facedetect`。检测坐标走 GStreamer bus
消息，经 `FaceBranch` 聊合（按面积降序取 top-N=8，叠加
`cooldown_ms` 节流）后以 `kind:"faces"` 的 NDJSON 行写入 events
FIFO。Web 控制台通过 `/ws/events` 订阅，在 `<video>` 上叠 canvas
实时画框 —— RTSP 主线本身从不带框(`display=false`)，录制 / 二
次编码不会把框英进去。

相关配置项均在 `face.*` 下：

| 配置项 | 含义 |
|---|---|
| `face.enabled` | 总开关。`false` 时 pipeline 完全不插入副线。 |
| `face.detect.cascade` | Haar XML 路径（一般 `haarcascade_frontalface_default.xml`）。 |
| `face.detect.min_size_px` | 检测矩形最小边长；越大越快、误检越少。 |
| `face.detect.scale_factor` / `min_neighbors` | 标准 OpenCV 多尺度 + 投票阈值。 |
| `face.rate.fps_limit` | 副线检测帧率（`videorate` 节流），默认 5，不建议调高。 |
| `face.control.enabled_at_start` | 启动时 `face_valve` 是否默认打开；true 即开机自检。 |
| `face.control.emit_when_empty` | count=0 时是否也上报（调试期建议开）。 |
| `face.control.cooldown_ms` | 同一事件的聚合窗口，防 bus 风暴。 |

events FIFO 的报文格式见
[docs/control/event_fifo.md](docs/control/event_fifo.md);element 本身的
写真见 [docs/gstreamer/facedetect.md](docs/gstreamer/facedetect.md)。

### PAG 叠加控制

`pagfilter` 元素允许在 pipeline 处于 `PLAYING` 时直接热更新,因此切
换素材、改文本图层、开关画中画都不需要重启守护进程。Web 控制台里
的 *pag overlay* 面板调用的就是下面这些命令。

```bash
# 查看叠加状态(attached / pag_file / replace_idx / replace_every)
./build/vm_iot_ctl pag get

# 热切 .pag 素材(绝对路径;守护进程原地重新加载)
./build/vm_iot_ctl pag set-file /abs/path/to/sticker.pag

# 用 UTF-8 字符串替换第 IDX 个文本图层
./build/vm_iot_ctl pag set-text 0 "你好,世界"

# 画中画:把当前视频帧塞进第 IDX 个 PAG 图层。-1 关闭。
./build/vm_iot_ctl pag set-replace-image 0
./build/vm_iot_ctl pag set-replace-image -1

# 画中画上行节流:每 N 帧推送 1 帧(N >= 1)。
# N 越大 CPU/GPU 占用越低,PAG 侧画面也越卡顿。
./build/vm_iot_ctl pag set-replace-image-every 2
```

构建期开关:libpag 是否真正参与链接由 CMake 配置阶段的
`-DVM_IOT_ENABLE_LIBPAG=ON` 控制。关闭时,`filter.pag.*` 这一段配
置仍然会被解析,但运行期会跳过 PAG 段(适合 CI / 最小化构建)。

---

## V4L2 能力探测

构建 pipeline 之前,守护进程会通过 `V4L2Prober` 列出摄像头能力,然
后丢给 `CapsRanker`,后者按照请求配置给每个 `(fmt, w, h, fps)` 组
合打分,选出最优解。同一段代码也以独立工具的形式暴露:

```bash
./build/probe_dev /dev/video0
```

可以用它来确认守护进程会选哪一组,或者跟
`v4l2-ctl --list-formats-ext -d /dev/video0` 做对比。

---

## 测试

测试默认会被构建,使用 GoogleTest,自动通过 `FetchContent` 拉取。

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

当前覆盖:`test_config`、`test_caps_ranker`、`test_pipeline_builder`、
`test_v4l2_prober`。新增测试只需在 `tests/` 下放一个 `test_*.cpp`
文件,`tests/CMakeLists.txt` 会自动收录。

---

## 辅助脚本

- `scripts/gst_launch/` —— 一组独立小脚本,封装常用的
  `gst-launch-1.0` pipeline:探测设备、本地预览、录制到文件、跑
  `test-launch`、dmabuf/GL 探测等等。它们与主二进制完全解耦,常用
  于在守护进程之外排查采集或编码问题。
- `scripts/bench/` —— 编码器基准测试(例如 `h265_compare.sh`)。

