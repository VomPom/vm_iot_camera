#!/usr/bin/env bash
# 启动 test-launch 包装的 RTSP server，复用 03_record.sh 的 $ENC_STR 拼装
set -euo pipefail

BACKEND="${BACKEND:-auto}"
DEV="${DEV:-/dev/video0}"
W="${W:-1280}"; H="${H:-720}"; FPS="${FPS:-30}"
BR="${BR:-4000}"; GOP="${GOP:-30}"
PORT="${PORT:-8554}"
MOUNT="${MOUNT:-/test}"
TEST_LAUNCH="${TEST_LAUNCH:-$HOME/work/test-launch}"

# 与 03_record 同样的 backend 选择逻辑（略，同上脚本 select_backend 函数）
source "$(dirname "$0")/_common.sh"   # 把 select_backend / ENC_STR 拼装抽到 _common.sh
ENC=$(select_backend)
build_enc_str "$ENC"                  # 设置 ENC_STR / SRC_FMT / PRE

CAPS="video/x-raw,format=$SRC_FMT,width=$W,height=$H,framerate=$FPS/1"

LAUNCH="( v4l2src device=$DEV
    ! $CAPS
    ${PRE:+! $PRE}
    ! $ENC_STR
    ! h264parse config-interval=1
    ! rtph264pay name=pay0 pt=96 mtu=1400 )"

echo "[info] launch = $LAUNCH"
echo "[info] stream at rtsp://0.0.0.0:$PORT$MOUNT"

# test-launch 默认 mount 是 /test, port 是 8554；用 -p / 改即可
"$TEST_LAUNCH" -p "$PORT" "$LAUNCH"