#!/usr/bin/env bash
# 探测当前系统的 V4L2 摄像头能力，把结果落到 logs/01_probe_<date>.log
set -euo pipefail

LOG_DIR="$(dirname "$0")/../../logs"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/01_probe_$(date +%Y%m%d_%H%M%S).log"

{
    echo "===== uname ====="
    uname -a

    echo
    echo "===== v4l2-ctl --list-devices ====="
    v4l2-ctl --list-devices || echo "no v4l2 device"

    echo
    for dev in /demv/video*; do
        [ -e "$dev" ] || continue
        echo "----- $dev caps -----"
        v4l2-ctl -d "$dev" --all          | sed -n '1,40p'
        echo "----- $dev formats -----"
        v4l2-ctl -d "$dev" --list-formats-ext
        echo
    done
} | tee "$LOG"

echo "[done] log saved to $LOG"