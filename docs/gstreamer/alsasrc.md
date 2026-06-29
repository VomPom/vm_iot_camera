# alsasrc

> 项目内位置：音频通道的"v4l2src"——`cfg.audio.enabled=true` 时 pipeline 的第一个元素，
> 把麦克风采到的 PCM 推到 `audioconvert ! audioresample ! volume(aud_vol) ! valve(aud_valve) ! tee(at)`。

## 1. 基本信息

| 项 | 值 |
|---|---|
| 分类 | **Source（音频）** |
| 所在插件 | `gstreamer1.0-alsa` |
| 全名 | `Audio source (ALSA)` |

`alsasrc` 直接和 ALSA 内核 PCM 设备打交道，旁路 PulseAudio。在 headless / Pi /
UTM aarch64 这类没有桌面会话的环境上是首选——它不依赖任何用户态守护进程。

### Pad 端口能力

- **src**：`audio/x-raw` 为主，常见 `format=S16LE`（USB mic 主流）。
- 实际能力由 ALSA 设备本身决定，下游用 `audio/x-raw,rate=...,channels=...` 显式约束。

### 关键属性

| 属性 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `device` | string | `default` | ALSA PCM name，如 `hw:0,0` / `plughw:CARD=USB,DEV=0` |
| `do-timestamp` | bool | `true` | 用 pipeline clock 给 buffer 打 PTS（**强烈建议保 true**） |
| `buffer-time` | uint64 | `200000`（µs） | 内核 ALSA 缓冲区总时长 |
| `latency-time` | uint64 | `10000`（µs） | 单次唤醒粒度（周期长度） |
| `provide-clock` | bool | `true` | 是否暴露 audio clock 给 pipeline 当主时钟 |

### 使用举例

```bash
# 单测：直接听麦克风
gst-launch-1.0 alsasrc device=hw:0,0 ! audioconvert ! autoaudiosink
```

### 项目内用法

```cpp
// pipeline_builder.cpp::build_audio_source_segment
os << "alsasrc";
if (a.capture.do_timestamp) os << " do-timestamp=true";
if (!a.capture.device.empty()) os << " device=" << a.capture.device;
os << " buffer-time=200000 latency-time=20000";
os << " ! audio/x-raw,rate=" << a.capture.samplerate
   << ",channels="           << a.capture.channels;
os << " ! audioconvert ! audioresample";
```

`200ms 总缓冲 / 20ms 周期` 是 USB mic 上验证过的保守值——更小会导致首 1~2 秒
偶发花屏/静默；更大会拉高端到端延迟。

## 2. 内部工作原理与数据流程

```mermaid
flowchart LR
    subgraph "ALSA 内核"
      H[硬件 / DMA]
      H --> R[ALSA ring buffer<br/>buffer-time=200ms]
    end
    subgraph "alsasrc 元素"
      R -->|周期 IRQ| W[snd_pcm_readi<br/>每 latency-time 读一周期]
      W --> P[包成 GstBuffer<br/>do-timestamp=true 打 PTS]
    end
    P --> Q[下游 audioconvert / queue]
```

核心机制：

1. **轮询周期**：内核以 `latency-time`（默认 10ms）为粒度产生 IRQ，alsasrc
   每次唤醒读出一段 PCM。
2. **时间戳**：`do-timestamp=true` 时每个 buffer 的 PTS = `pipeline clock 当前值`。
   这是音视频同步的根。
3. **provide-clock**：默认会把 alsa 自己的硬件时钟暴露给 pipeline。本项目让
   pipeline 用默认 monotonic clock，**不强行 set_clock**，让 GStreamer 自动选。

## 3. 性能开销与其他补充

### 性能特征

- **CPU 开销极低**：48k 立体声 16bit ≈ 192 KB/s，alsasrc 自己的开销可以忽略。
- **延迟**：稳定状态下 = `latency-time`（20ms）；首次起播延迟 ≈ `buffer-time`（200ms）。
- **内存**：`buffer-time` 决定的内核 ring buffer，几十 KB 量级。

### 与 PulseAudio 的冲突

PulseAudio 默认会把所有 ALSA 设备"独占"。在桌面 Linux 上跑 alsasrc 经常报
`-EBUSY`。解决方案：

- 服务器/headless：直接用 alsasrc，不装 pulseaudio。
- 桌面调试：用 `pulsesrc`（项目里 backend=pulse 占坑），或 `systemctl --user mask pulseaudio`。

### 设备名（device）解析

| 形式 | 说明 |
|---|---|
| `default` | ALSA 默认设备（看 `~/.asoundrc` / `/etc/asound.conf`） |
| `hw:0,0` | 第 0 张声卡的第 0 个 PCM；**绕过 plug，采样率/通道必须设备原生支持** |
| `plughw:0,0` | 同上但带自动重采样/通道转换；省心但额外 CPU |
| `plughw:CARD=USB,DEV=0` | 按 card name（`/proc/asound/cards` 显示） |

项目默认 `hw:0,0`：低延迟、省 CPU；前提是 yaml 的 rate/channels 与设备原生
能力对齐（USB mic 主流 48k/2ch 都能直接命中）。

### 常见坑

1. **buffer-time 太小** → 首次起播 audio_out 来不及，下游 queue 饿死，前几秒静默。
2. **device 写错** → snd_pcm_open 返 ENOENT；项目用 alsa_prober 在启动期挡掉。
3. **sample rate 设备不原生支持** → `hw:` 路径会 caps 协商失败；改用 `plughw:` 或调 yaml。
4. **同一设备被 pulseaudio 抢占** → -EBUSY；要么停 pulse，要么走 pulsesrc。
5. **`provide-clock=true` + 录像 mp4mux** → 时基切换可能让 mp4 起始 PTS 偏移；
   本项目不录音视频混流时不踩这个坑。
