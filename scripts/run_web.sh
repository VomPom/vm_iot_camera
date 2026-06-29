#!/usr/bin/env bash
# scripts/run_web.sh —— 开发期一键起 vm_iot web 控制台
# ---------------------------------------------------------------------------
# 步骤：
#   1. 检查 daemon 的 FIFO 是否就绪（不就绪打印提示但仍启动 web，便于排错）
#   2. 后台拉起 mediamtx（前提：./third_party/mediamtx/mediamtx 二进制已下载）
#   3. 前台跑 node web/server.mjs
#   4. Ctrl-C 时连带停止 mediamtx
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WEB_DIR="$ROOT/web"
MTX_DIR="$ROOT/third_party/mediamtx"
MTX_BIN="$MTX_DIR/mediamtx"
MTX_CFG="$MTX_DIR/mediamtx.yml"

CTL_FIFO="${IOTCAM_CTL:-/tmp/vm_iot.ctl}"
REPLY_FIFO="${IOTCAM_REPLY:-/tmp/vm_iot.reply}"

echo "[run_web] checking daemon FIFOs..."
if [[ ! -p "$CTL_FIFO" ]]; then
  echo "  WARN  $CTL_FIFO not found (or not a FIFO). is vm_iot daemon running?"
fi
if [[ ! -p "$REPLY_FIFO" ]]; then
  echo "  WARN  $REPLY_FIFO not found (or not a FIFO)."
fi

echo "[run_web] checking mediamtx..."
if [[ ! -x "$MTX_BIN" ]]; then
  cat <<EOF
  ERROR mediamtx binary not found at $MTX_BIN
  please download once:
    cd $MTX_DIR
    # for raspi5 (aarch64):
    wget https://github.com/bluenviron/mediamtx/releases/latest/download/mediamtx_linux_arm64v8.tar.gz
    tar xzf mediamtx_linux_arm64v8.tar.gz
    chmod +x mediamtx
EOF
  exit 1
fi

echo "[run_web] checking node deps..."
if [[ ! -d "$WEB_DIR/node_modules" ]]; then
  echo "  installing npm deps (one-time)..."
  ( cd "$WEB_DIR" && npm install --omit=dev )
fi

echo "[run_web] launching mediamtx in background..."
"$MTX_BIN" "$MTX_CFG" &
MTX_PID=$!
trap 'echo "[run_web] stopping mediamtx ($MTX_PID)"; kill $MTX_PID 2>/dev/null || true' EXIT INT TERM

# 给 mediamtx 1s 启动时间再起 web
sleep 1

echo "[run_web] launching web console (foreground)"
echo "          http://$(hostname -I 2>/dev/null | awk '{print $1}'):${VM_IOT_WEB_PORT:-8080}"
cd "$WEB_DIR"
exec node server.mjs
