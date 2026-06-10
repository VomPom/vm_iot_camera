#!/usr/bin/env bash
# 选择编码器后端并落盘 mp4，按 num-buffers=300 录约 10s（30fps）
set -euo pipefail

BACKEND="${BACKEND:-auto}"        # x264 | openh264 | x265 | auto
DEV="${DEV:-/dev/video0}"
W="${W:-1280}"; H="${H:-720}"; FPS="${FPS:-30}"
BR="${BR:-4000}"                  # kbps
GOP="${GOP:-30}"
OUT="${OUT:-test_${BACKEND}.mp4}"

# 统一从 _common.sh 取后端选择 / SRC_FMT / ENC_STR / PRE，避免与 rtsp.sh、preview.sh 维护两份
source "$(dirname "$0")/_common.sh"

ENC=$(select_backend)
echo "[info] using encoder backend: $ENC"

build_enc_str "$ENC"              # 设置 ENC_STR / SRC_FMT / PRE

CAPS="video/x-raw,format=$SRC_FMT,width=$W,height=$H,framerate=$FPS/1"

# x265 走 H.265 链路（h265parse），其余走 H.264 链路。qtmux 同时支持两者。
case "$ENC" in
  x265) PARSE="h265parse";;
  *)    PARSE="h264parse";;
esac

gst-launch-1.0 -e v4l2src device="$DEV" num-buffers=$((FPS*10)) \
    ! "$CAPS" \
    ${PRE:+! $PRE} \
    ! $ENC_STR \
    ! $PARSE ! qtmux ! filesink location="$OUT"

echo "[done] -> $OUT"
ffprobe -v error -show_streams "$OUT" | grep -E 'codec_name|width|height|bit_rate|profile|has_b_frames'