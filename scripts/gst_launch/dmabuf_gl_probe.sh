#!/usr/bin/env bash
# 仅验证 dmabuf <-> GL <-> dmabuf 链路是否能 caps 协商成功
set -euo pipefail

DEV="${DEV:-/dev/video0}"
W="${W:-1280}"; H="${H:-720}"; FPS="${FPS:-30}"

GST_DEBUG=2,glupload:6,gldownload:6 \
gst-launch-1.0 -v \
    v4l2src device="$DEV" io-mode=4 num-buffers=60 \
    ! "video/x-raw(memory:DMABuf),format=NV12,width=$W,height=$H,framerate=$FPS/1" \
    ! glupload ! glcolorconvert ! gldownload \
    ! "video/x-raw(memory:DMABuf),format=NV12" \
    ! fakesink dump=false silent=false