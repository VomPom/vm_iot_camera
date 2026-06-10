select_backend() {
    if [ "${BACKEND:-auto}" != "auto" ]; then echo "$BACKEND"; return; fi
    # 仅在三种纯软编里挑：优先 x264（兼容/性能折中最优），其次 openh264，最后 x265。
    if gst-inspect-1.0 x264enc     >/dev/null 2>&1; then echo x264;     return; fi
    if gst-inspect-1.0 openh264enc >/dev/null 2>&1; then echo openh264; return; fi
    if gst-inspect-1.0 x265enc     >/dev/null 2>&1; then echo x265;     return; fi
    echo x264
}

# 根据后端给出最合适的 v4l2src 采集格式（不涉及编码），preview/record/rtsp 共用
select_src_fmt() {
    local enc="$1"
    case "$enc" in
      x264|openh264|x265) echo I420;;
      *)                  echo I420;;
    esac
}

build_enc_str() {
    local enc="$1"
    BR="${BR:-4000}"; GOP="${GOP:-30}"
    SRC_FMT="$(select_src_fmt "$enc")"
    case "$enc" in
      x264)
        ENC_STR="x264enc tune=zerolatency speed-preset=ultrafast bitrate=$BR key-int-max=$GOP bframes=0"
        PRE="videoconvert";;
      openh264)
        # openh264 用 bps 单位
        ENC_STR="openh264enc bitrate=$((BR*1000)) gop-size=$GOP complexity=low rate-control=bitrate"
        PRE="videoconvert";;
      x265)
        ENC_STR="x265enc tune=zerolatency speed-preset=ultrafast bitrate=$BR key-int-max=$GOP"
        PRE="videoconvert";;
    esac
    export ENC_STR SRC_FMT PRE
}