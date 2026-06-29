# vm_iot web console

浏览器可视化控制台，替代 `ffplay rtsp://...` + `iotcamctl`。

**重要：本目录不修改 daemon 任何代码**，所有控制走 daemon 的 FIFO 控制通道
（`/tmp/vm_iot.ctl` + `/tmp/vm_iot.reply`），画面通过 [mediamtx](https://github.com/bluenviron/mediamtx)
把 RTSP 转成浏览器原生支持的 WebRTC / HLS。

## 架构

```
浏览器 (Chrome / Safari / Firefox)
   │
   ├── http://pi:8080/            ← 静态页面 + REST + WebSocket（Node 进程）
   │       │
   │       ├─ POST /api/filter ─┐
   │       ├─ POST /api/snapshot┤
   │       └─ ...               │
   │                            ▼
   │                       FifoClient
   │                     ┌──────────┐
   │                     │ /tmp/vm_iot.ctl   ──► daemon
   │                     │ /tmp/vm_iot.reply ◄── daemon
   │                     └──────────┘
   │
   └── http://pi:8889/live/whep   ← WebRTC（mediamtx 进程）
       http://pi:8888/live/...    ← HLS 兜底
                  ▲
                  │ rtsp://127.0.0.1:8554/live
                  │
              vm_iot daemon (不动)
```

## 三个独立进程

| 进程              | 端口             | 启动                                       |
|-------------------|------------------|--------------------------------------------|
| vm_iot daemon     | 8554 (RTSP)      | 你已有的 `vm_iot -c assets/config/default.yaml` |
| mediamtx          | 8889 / 8888      | `./third_party/mediamtx/mediamtx ./third_party/mediamtx/mediamtx.yml` |
| node web 控制台   | 8080             | `cd web && npm install && npm start`       |

挂掉互不影响，daemon 优先。

## Pi5 一次性环境准备

```bash
# Node 18+
sudo apt install -y nodejs npm
node --version    # 期望 >= 18

# mediamtx 二进制（aarch64）
cd third_party/mediamtx
wget https://github.com/bluenviron/mediamtx/releases/latest/download/mediamtx_linux_arm64v8.tar.gz
tar xzf mediamtx_linux_arm64v8.tar.gz
chmod +x mediamtx
./mediamtx --version

# Web 依赖
cd ../../web
npm install --omit=dev
```

## 开发期一键启动

确保 daemon 已经在跑（`/tmp/vm_iot.ctl` 已经存在），然后：

```bash
./scripts/run_web.sh
```

脚本会：
1. 检查 FIFO 与 mediamtx 二进制；
2. 后台拉起 mediamtx；
3. 前台跑 node 服务；
4. Ctrl-C 时连带杀 mediamtx。

打开 `http://<pi-ip>:8080/`，应当看到画面 + 控制面板。

## 生产部署：systemd

`/etc/systemd/system/mediamtx.service`：

```ini
[Unit]
Description=mediamtx (RTSP→WebRTC bridge for vm_iot)
After=network-online.target vm_iot.service
Wants=network-online.target

[Service]
Type=simple
ExecStart=/opt/vm_iot/third_party/mediamtx/mediamtx /opt/vm_iot/third_party/mediamtx/mediamtx.yml
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
```

`/etc/systemd/system/vm_iot_web.service`：

```ini
[Unit]
Description=vm_iot web console
After=network-online.target vm_iot.service
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/vm_iot/web
ExecStart=/usr/bin/node server.mjs
Environment=IOTCAM_CTL=/tmp/vm_iot.ctl
Environment=IOTCAM_REPLY=/tmp/vm_iot.reply
Environment=VM_IOT_WEB_HOST=0.0.0.0
Environment=VM_IOT_WEB_PORT=8080
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
```

启用：
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now mediamtx vm_iot_web
sudo systemctl status vm_iot_web
```

## 常见问题

| 现象 | 排查 |
|---|---|
| 顶栏一直 `daemon offline` | daemon 没起 / FIFO 路径不一致；检查 `assets/config/default.yaml` 中 `control.request_fifo` |
| 视频区 `webrtc failed` | mediamtx 没起 / 防火墙挡 8889；试试切到 HLS |
| 切到 HLS 仍黑屏 | mediamtx 拉不到 daemon 的 RTSP；`./mediamtx mediamtx.yml` 在前台跑看日志 |
| `EACCES: open /tmp/vm_iot.ctl` | 用户没有 FIFO 写权限；把 web 服务跑成和 daemon 同一个用户 |
| 移动端布局错乱 | 控制台只保证 ≥1024 宽度可读，移动端非目标场景 |

## 安全说明

- 默认监听 `0.0.0.0:8080`，**未做鉴权**；外网访问请加反向代理 + basic auth 或 SSH 隧道。
- `POST /api/raw` 可发送任意 daemon 协议命令行，仅做长度与换行清洗，daemon 端会再校验。
- 所有命令通过 daemon ControlChannel 走二次校验，前端不能绕过。

## 文件清单

```
web/
├── server.mjs          # Fastify 入口
├── fifo.mjs            # FIFO 客户端（协议状态机 + 命令队列）
├── routes.mjs          # REST + WS 路由 + 命令白名单
├── package.json        # 三个依赖：fastify / @fastify/static / @fastify/websocket
├── README.md           # 你正在读的这份
└── static/
    ├── index.html      # 单页骨架
    ├── app.js          # 媒体接入 + 命令分发 + 状态推送
    └── style.css       # 工业控制台主题

third_party/mediamtx/mediamtx.yml   # mediamtx 配置（仅 WebRTC + HLS）
scripts/run_web.sh                  # 开发期一键启动
```
