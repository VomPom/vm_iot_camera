# avenc_aac

> 项目内位置：voaacenc 缺失时的 AAC 编码器 fallback。
> `gstreamer1.0-plugins-bad` 没装时由 PipelineBuilder 在启动期自动切到本元素。

## 1. 基本信息

| 项 | 值 |
|---|---|
| 分类 | **Encoder（音频）** |
| 所在插件 | `gstreamer1.0-libav` |
| 全名 | `libav AAC encoder`（FFmpeg 内置 AAC encoder 的 GStreamer wrapper） |

`avenc_aac` 是 FFmpeg 项目里的 AAC LC 编码器（由 ffmpeg 主仓维护），相比
voaacenc 实现更新、参数更多，但 GStreamer 对其属性暴露偏少。

### Pad 端口能力

- **sink**：`audio/x-raw, format={S16LE,FLTP}, rate={8000~96000}, channels={1,2,...,8}`。
- **src**：`audio/mpeg, mpegversion=4, stream-format=raw`。

与 voaacenc 输出 caps **完全等价**——下游 `aacparse ! rtpmp4apay` 链路无须改动。

### 关键属性

| 属性 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `bitrate` | int | `128000` | bps |
| `compliance` | enum | `normal` | normal/strict/experimental，默认即可 |
| 其他 ffmpeg 通用 | string | - | `avenc_aac` 暴露的私有属性较少，绝大多数走默认 |

### 使用举例

```bash
gst-launch-1.0 audiotestsrc ! audio/x-raw,rate=48000,channels=2 \
  ! audioconvert ! avenc_aac bitrate=96000 ! aacparse ! filesink location=t.aac
```

### 项目内用法

```cpp
// pipeline_builder.cpp::build()
GstElementFactory* f = gst_element_factory_find("voaacenc");
if (!f) {
    LOGW("audio: voaacenc factory not found, falling back to avenc_aac.");
    eff_enc.aac_impl = AudioAacImpl::AvEnc;
}
// audio_encoder_str() 据 aac_impl 输出 "avenc_aac bitrate=..."
```

fallback 后的 launch 字符串与 voaacenc 版本只有元素名差异，其他完全一致。

## 2. 内部工作原理与数据流程

与 voaacenc 共享同一套 AAC LC 流程（MDCT → 心理声学 → 量化 → 熵编码），仅
具体实现差异。

```mermaid
flowchart LR
    I[PCM<br/>S16LE / 48k] --> F[1024-sample 帧]
    F --> L[FFmpeg AAC encoder<br/>含 lookahead]
    L --> O[AAC raw 帧]
```

差异点：

- **lookahead**：avenc_aac 内部有 1 帧的 lookahead，启动延迟比 voaacenc 多 ~21ms。
- **量化策略**：FFmpeg 的 PE 模型在中码率（96-128k）质量略优。

## 3. 性能开销与其他补充

### 性能特征

- **CPU**：单核 ~3-5%（48k/2ch/96k），比 voaacenc 略高。
- **延迟**：编码端 ~63ms（多 1 帧 lookahead）。
- **内存**：~200KB。

### 何时会被启用

| 场景 | 是否触发 fallback |
|---|---|
| 标准 Ubuntu/Debian + plugins-bad | 否 |
| 精简镜像只装了 libav | **是** |
| Buildroot/Yocto 自定义、disable bad | **是** |

### 常见坑

1. **bitrate 单位是 bps** → 与 voaacenc 一致，项目用 kbps×1000。
2. **plugins-bad 与 libav 都缺** → 启动期 launch 解析失败，gst-rtsp-server
   会在 client 连接瞬间报错；建议在镜像构建里至少装一个。
3. **avenc_aac 的某些 ffmpeg 版本不接 strict-1（experimental）** → 项目不踩。
4. **VBR vs CBR**：avenc_aac 默认 ABR，与 voaacenc 一致；不要试图开 VBR，
   RTSP 客户端体验会变差。
