# rtpopuspay

> 项目内位置：当 `cfg.audio.encoder.backend=opus` 时替代 `rtpmp4apay` 的 RTP 打包器。
> 元素名同样是 `pay1`，gst-rtsp-server 自动列入 SDP 第二路。

## 1. 基本信息

| 项 | 值 |
|---|---|
| 分类 | **Payloader（RTP）** |
| 所在插件 | `gstreamer1.0-plugins-good`（`rtp`） |
| 全名 | `RTP Opus payloader` |

按 **RFC 7587** 把 Opus packet 打成 RTP。SDP fmtp 极简：

```
a=rtpmap:97 opus/48000/2
a=fmtp:97 sprop-stereo=1
```

### Pad 端口能力

- **sink**：`audio/x-opus`（来自 opusenc）。
- **src**：`application/x-rtp, media=audio, clock-rate=48000, encoding-name=OPUS`。

`clock-rate=48000` 是 Opus 的固定取值——**RTP 头时间戳永远以 48k 为单位**，
即使内部用 24k/16k 编码（这是 RFC 7587 的硬性规定）。

### 关键属性

| 属性 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `pt` | int | `96` | RTP payload type；项目用 `97` 与 `pay0` 错开 |
| `mtu` | int | `1400` | UDP MTU |
| `name` | string | (auto) | 项目用 `pay1` |
| `dtx` | bool | `false` | Discontinuous Transmission，弱网省带宽，默认关 |

### 使用举例

```bash
gst-launch-1.0 audiotestsrc ! opusenc ! rtpopuspay pt=97 \
  ! udpsink host=127.0.0.1 port=5004
```

### 项目内用法

```cpp
// pipeline_builder.cpp::append_branch_audio_main_rtp
const char* payer = (backend == AudioEncoderBackend::AAC)
                        ? "rtpmp4apay" : "rtpopuspay";
os << " enc_at. ! queue ... ! " << payer << " name=pay1 pt=97 mtu=1400";
```

注意 Opus 路径**不需要 aacparse**，opusenc 输出直接接 rtpopuspay 即可。

## 2. 内部工作原理与数据流程

```mermaid
flowchart LR
    I[Opus packet<br/>来自 opusenc] --> P[一帧一包<br/>opus 一个 packet 一定 ≤ MTU]
    P --> H[加 RTP 头<br/>pt=97 ts=PTS×48000<br/>marker=0]
    H --> O[RTP 包]
```

核心机制：

1. **一对一打包**：Opus packet 设计上就是 < MTU，无需 fragmentation。
2. **clock-rate 固定 48k**：即使内部 SR 是 16k，RTP ts 仍按 48k 推进。
3. **stereo 标记**：`sprop-stereo` 由 caps 自动决定，客户端据此选立体声/单声。

## 3. 性能开销与其他补充

### 性能特征

- **CPU**：< 0.1%。
- **延迟**：0（同步元素）。
- **内存**：每包临时 ~MTU。

### 与 rtpmp4apay 的对比

| 维度 | rtpmp4apay | rtpopuspay |
|---|---|---|
| SDP fmtp 复杂度 | 中（config/SizeLength/IndexLength...） | 极低（sprop-stereo） |
| 客户端首屏延迟 | 解析 fmtp 再请头 ~50ms | 直接解码 ~5ms |
| 弱网 DTX 支持 | 无 | 有（dtx=true） |
| mp4 录像 | ✅ | ❌（必须 mkv） |
| 项目默认 | ✅ | 备选 |

### 何时切到 Opus

- 实时性优先（监控对讲、双向 push-to-talk）：Opus 端到端 < 30ms。
- 严格带宽预算：Opus 32k 已能听清人声，AAC 需要 48k+。
- 不需要 mp4 录像：客户端不抱怨 mkv 的话切 Opus 更划算。

### 常见坑

1. **mp4mux 接 Opus** → mp4 不允许 Opus track，必须 mkv/webm。
2. **dtx=true + RTSP** → 客户端 jitter buffer 会偶发饿死；本项目默认关。
3. **某些老设备的 RTSP/SDP 解析对 OPUS 不识别** → 回退 AAC。
4. **clock-rate=48000 是协议要求** → 不要试图改成 24k，VLC 会直接静音。
