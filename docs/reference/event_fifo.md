# event_fifo：daemon → web 单向事件流

## 定位

`vm_iot` 有两条 **完全解耦** 的 FIFO 通道：

| 通道 | 方向 | 语义 | 端点 |
|---|---|---|---|
| control | 双向（req/reply） | 请求-应答；web/CLI 发命令、daemon 回执行结果 | `/tmp/vm_iot.ctl` + `/tmp/vm_iot.reply` |
| **event_fifo** | 单向（daemon → 外） | 事件广播；daemon 主动上报状态变化 | `/tmp/vm_iot.events` |

命令通道是"你问我答"的强顺序模式，事件通道则是"我说你听"的高频广播模式。
把两者拆开可以避免异步事件挤占应答队列的时序，也让事件流的读端可以自由热接热断。

## 消息格式

**NDJSON**（Newline-Delimited JSON）——一行一个 JSON 对象，以 `\n` 分隔。
消费端直接按行 split 即可；行内容不做任何 escape 之外的处理。

### `kind: "faces"` — 人脸检测帧

由 [`FaceBranch`](../../src/branches/face/face_branch.cpp) 在每次通过 cooldown / emit_when_empty
过滤后的检测事件产生，透过 [`EventFifoWriter`](../../src/control/event_fifo_writer.cpp) 写入 FIFO：

```json
{
  "kind": "faces",
  "ts_ns": 12345678901234,
  "frame_w": 1280,
  "frame_h": 720,
  "count": 2,
  "rects": [
    {"x": 320, "y": 180, "w": 160, "h": 160},
    {"x": 720, "y": 200, "w": 140, "h": 140}
  ]
}
```

字段语义：

- `ts_ns`：daemon 侧 `steady_clock` 单调时间戳（**非 wall clock**）；仅用于相对推算，
  跨机比较无意义。
- `frame_w` / `frame_h`：**主线采集帧尺寸**（对应 `cfg.capture.width/height`）；
  `rects` 里的坐标就在此尺寸下，前端做归一化时以此为基。
- `count`：本次检测到的人脸总数；上限由 daemon 侧裁到 8。
- `rects`：矩形数组，每项含 `x/y/w/h` 四个整数（左上角原点）。已按面积降序。

## 语义约束

1. **无 ACK / 无重传**：写端不知道谁在读；管道满或无读端时 daemon 直接丢并按秒级
   节流打 warn，绝不阻塞检测热路径。
2. **可断连恢复**：读端可以随时 `close` / `open`，daemon 不感知；期间产生的事件
   可能丢失（人脸检测本就是"最新一帧最重要"的场景，不做补偿）。
3. **顺序保序**：daemon 侧按发生顺序 `write`，读端按行读，保序自然成立。
4. **未来可扩展 kind**：新增 `kind` 时消费端要按 `msg.kind` 分发，未识别 kind 应静默丢弃，
   避免耦合。

## 手动验证

daemon 起来后，先看 FIFO 是否被创建：

```bash
ls -l /tmp/vm_iot.events
# prw-rw-rw- 1 pi pi 0 ... /tmp/vm_iot.events
```

用 `cat` 直接观察实时事件流：

```bash
cat /tmp/vm_iot.events
# {"kind":"faces","ts_ns":...,"frame_w":1280,"frame_h":720,"count":1,"rects":[{...}]}
# {"kind":"faces",...}
# ...
```

再用 `jq` 只看 count：

```bash
cat /tmp/vm_iot.events | jq -c '{ts_ns, count}'
```

## 配置

`assets/config/default.yaml`：

```yaml
control:
  request_fifo: /tmp/vm_iot.ctl
  reply_fifo:   /tmp/vm_iot.reply
  event_fifo:   /tmp/vm_iot.events   # 空串则不启用事件广播
```

Node web 侧对应环境变量：`IOTCAM_EVENTS`（见 [`web/README.md`](../../web/README.md)）。

## 相关代码

- daemon 写端：[`src/control/event_fifo_writer.h/.cpp`](../../src/control/event_fifo_writer.h)
- daemon 触发点：[`src/branches/face/face_branch.cpp::on_bus_message`](../../src/branches/face/face_branch.cpp)
  中回调 `on_faces_cb_`；由 `main.cpp` 组装。
- web 读端：[`web/event_fifo.mjs`](../../web/event_fifo.mjs)
- web 广播 → 前端：[`web/routes.mjs`](../../web/routes.mjs) 里 `/ws/events` 桥接
- 前端画框消费：[`web/static/app.js`](../../web/static/app.js) 中的 `faceOverlay` 模块
