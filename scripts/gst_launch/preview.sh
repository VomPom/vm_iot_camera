#!/usr/bin/env bash
# 最简 capture -> display, 用来确认硬件出图正常
set -euo pipefail

DEV="${1:-/dev/video0}"
W="${2:-1280}"
H="${3:-720}"
FPS="${4:-30}"

# 优先尝试 YUY2
gst-launch-1.0 -v \
    v4l2src device="$DEV" \
    ! "video/x-raw,format=YUY2,width=$W,height=$H,framerate=$FPS/1" \
    ! videoconvert \
    ! autovideosink sync=false

# 进阶：MJPEG 摄像头
# 1080p 通过 USB2.0 时 raw YUY2 带宽不够（约 590MB/s 远超 USB2 的 60MB/s 实际可用），必须走 MJPEG：
#gst_launch-launch-1.0 -v \
 #    v4l2src device=/dev/video0 \
 #    ! "image/jpeg,width=1920,height=1080,framerate=30/1" \
 #    ! jpegdec \
 #    ! videoconvert ! autovideosink sync=false