select_backend() {
    if [ "${BACKEND:-auto}" != "auto" ]; then echo "$BACKEND"; return; fi
    if gst-inspect-1.0 vaapih264enc      >/dev/null 2>&1; then echo vaapi;   return; fi
    if gst-inspect-1.0 nvh264enc         >/dev/null 2>&1; then echo nvenc;   return; fi
    if gst-inspect-1.0 v4l2h264enc       >/dev/null 2>&1; then echo v4l2m2m; return; fi
    echo x264
}

build_enc_str() {
    local enc="$1"
    BR="${BR:-4000}"; GOP="${GOP:-30}"
    case "$enc" in
      vaapi)
        ENC_STR="vaapih264enc rate-control=cbr bitrate=$BR keyframe-period=$GOP tune=low-power"
        SRC_FMT="YUY2"; PRE="videoconvert ! video/x-raw,format=NV12";;
      nvenc)
        ENC_STR="nvh264enc preset=low-latency-hp rc-mode=cbr bitrate=$BR gop-size=$GOP"
        SRC_FMT="YUY2"; PRE="videoconvert ! video/x-raw,format=NV12";;
      v4l2m2m)
        ENC_STR="v4l2h264enc extra-controls=\"controls,h264_profile=4,video_bitrate=$((BR*1000)),h264_i_frame_period=$GOP\""
        SRC_FMT="NV12"; PRE="";;
      x264)
        ENC_STR="x264enc tune=zerolatency speed-preset=ultrafast bitrate=$BR key-int-max=$GOP bframes=0"
        SRC_FMT="YUY2"; PRE="videoconvert";;
    esac
    export ENC_STR SRC_FMT PRE
}