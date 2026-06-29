# voaacenc

> 项目内位置：默认 AAC 编码器（`cfg.audio.encoder.backend=aac` & `aac_impl=voaacenc`）。
> 启动期 PipelineBuilder 工厂探测，缺失则 fallback 到 `avenc_aac`。

## 1. 基本信息

| 项 | 值 |
|---|---|
| 分类 | **Encoder（音频）** |
| 所在插件 | `gstreamer1.0-plugins-bad`（`voaacenc`） |
| 全名 | `Voice Age AAC Encoder` |

底层是 **VisualOn AAC**（Android 同款），LC profile，纯整数运算，
ARM/x86 上都很快。在嵌入式与 RTSP 场景里是最常见的 AAC 编码器。

### Pad 端口能力

- **sink**：`audio/x-raw, format=S16LE, layout=interleaved, rate={8000,11025,...,48000}, channels={1,2}`。
- **src**：`audio/mpeg, mpegversion=4, stream-format=raw, profile=lc, framed=true`。

`stream-format=raw` 指无任何容器/ADTS 包头的纯 AAC 帧；走 RTSP 时由
`aacparse` 把 codec_data（AudioSpecificConfig）补齐再丢给 `rtpmp4apay`。

### 关键属性

| 属性 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `bitrate` | int | `128000` | bps；项目用 `bitrate_kbps * 1000`，默认 96k |
| `tune` | enum | (无) | voaacenc 不暴露 tune |

### 使用举例

```bash
gst-launch-1.0 audiotestsrc ! audio/x-raw,rate=48000,channels=2 \
  ! audioconvert ! voaacenc bitrate=96000 ! aacparse \
  ! filesink location=test.aac
```

### 项目内用法

```cpp
// pipeline_builder.cpp::audio_encoder_str
"voaacenc bitrate=96000"   // bitrate_kbps=96 时

// build() 启动期 fallback：
GstElementFactory* f = gst_element_factory_find("voaacenc");
if (!f) {
    eff_enc.aac_impl = AudioAacImpl::AvEnc;   // 切到 avenc_aac
}
```

参数选择参考：

| 场景 | bitrate_kbps | 备注 |
|---|---|---|
| 监控音轨（人声） | 64 | 完全够用 |
| 默认 | 96 | 立体声音乐基本无差 |
| 高保真 | 128 | RTSP 上限，更多浪费 |

## 2. 内部工作原理与数据流程

```mermaid
flowchart LR
    I[输入 PCM<br/>S16LE / 48k / 2ch] --> F[1024-sample 帧累积]
    F --> M[MDCT 变换]
    M --> P[心理声学模型<br/>masking 阈值]
    P --> Q[量化 / 熵编码]
    Q --> O[输出 AAC raw 帧<br/>每帧 1024 样本 ≈ 21.3ms@48k]
```

核心机制：

1. **固定帧长 1024 样本**：48k 时每帧 ~21.3ms，是 AAC 协议规定。
   第一个输出帧需要至少 2 帧 PCM 缓冲（前后向窗），首包延迟 ~42ms。
2. **CBR 工作模式**：按 `bitrate` 严格平均，不做 VBR。心跳稳定的码率适合 RTSP。
3. **stream-format=raw**：voaacenc 输出最干净的 AAC，下游必须有 `aacparse`
   补齐 codec_data，否则 RTP 解析端拿不到 sample rate / profile。

## 3. 性能开销与其他补充

### 性能特征

- **CPU**：单核 ~1-2%（48k/2ch/96k）。
- **延迟**：编码端 ~42ms（双窗口）。
- **内存**：~100KB。

### 与 avenc_aac 的对比

| 维度 | voaacenc | avenc_aac |
|---|---|---|
| 实现 | VisualOn（C，定点） | FFmpeg（C，浮点） |
| 质量 | LC，中等 | LC/HE，更优（有 lookahead） |
| CPU | 略低 | 略高（5~10%） |
| 镜像可用性 | plugins-bad | libav |
| 项目首选 | ✅ | fallback |

镜像里通常 plugins-bad 与 libav 至少有一个；fallback 自动覆盖。

### 常见坑

1. **不接 aacparse 直连 rtpmp4apay** → 解析端报"missing sample-rate"，必须走 aacparse。
2. **bitrate 太低（<32k）** → voaacenc 内部会 clip 到 32k，warn 一行；项目默认 96k 不踩。
3. **sample rate 非 8/11/12/16/22/24/32/44.1/48 之一** → caps 协商失败；
   项目 yaml 校验已挡。
4. **mono → stereo 通过 audioconvert 上混** → 没问题；但 stereo 设备拉 mono 会
   被简单"相加 / 2"，不是真正 mid/side。
