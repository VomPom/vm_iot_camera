#!/usr/bin/env bash
# 选择编码器后端并落盘 mp4，按 num-buffers=300 录约 10s（30fps）
set -euo pipefail

BACKEND="${BACKEND:-auto}"        # vaapi | nvenc | v4l2m2m | x264 | auto
DEV="${DEV:-/dev/video0}"
W="${W:-1280}"; H="${H:-720}"; FPS="${FPS:-30}"
BR="${BR:-4000}"                  # kbps
GOP="${GOP:-30}"
OUT="${OUT:-test_${BACKEND}.mp4}"

select_backend() {
    if [ "$BACKEND" != "auto" ]; then echo "$BACKEND"; return; fi
    if gst-inspect-1.0 vaapih264enc      >/dev/null 2>&1; then echo vaapi;   return; fi
    if gst-inspect-1.0 nvh264enc         >/dev/null 2>&1; then echo nvenc;   return; fi
    if gst-inspect-1.0 v4l2h264enc       >/dev/null 2>&1; then echo v4l2m2m; return; fi
    echo x264
}

ENC=$(select_backend)
echo "[info] using encoder backend: $ENC"

case "$ENC" in
  vaapi)
    ENC_STR="vaapih264enc rate-control=cbr bitrate=$BR keyframe-period=$GOP tune=low-power"
    SRC_FMT="YUY2"
    PRE="videoconvert ! video/x-raw,format=NV12"
    ;;
  nvenc)
    ENC_STR="nvh264enc preset=low-latency-hp rc-mode=cbr bitrate=$BR gop-size=$GOP"
    SRC_FMT="YUY2"
    PRE="videoconvert ! video/x-raw,format=NV12"
    ;;
  v4l2m2m)
    ENC_STR="v4l2h264enc extra-controls=\"controls,h264_profile=4,h264_level=10,video_bitrate=$((BR*1000)),h264_i_frame_period=$GOP\""
    SRC_FMT="NV12"
    PRE=""
    ;;
  x264)
    ENC_STR="x264enc tune=zerolatency speed-preset=ultrafast bitrate=$BR key-int-max=$GOP bframes=0"
    SRC_FMT="YUY2"
    PRE="videoconvert"
    ;;
esac

CAPS="video/x-raw,format=$SRC_FMT,width=$W,height=$H,framerate=$FPS/1"

gst-launch-1.0 -e v4l2src device="$DEV" num-buffers=$((FPS*10)) \
    ! "$CAPS" \
    ${PRE:+! $PRE} \
    ! $ENC_STR \
    ! h264parse ! qtmux ! filesink location="$OUT"

echo "[done] -> $OUT"
ffprobe -v error -show_streams "$OUT" | grep -E 'codec_name|width|height|bit_rate|profile|has_b_frames'