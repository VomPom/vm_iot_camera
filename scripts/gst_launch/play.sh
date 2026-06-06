#!/usr/bin/env bash
URL="${1:-rtsp://127.0.0.1:8554/test}"
TRANSPORT="${TRANSPORT:-udp}"   # udp | tcp

case "$TRANSPORT" in
  udp)
    ffplay -fflags nobuffer -flags low_delay \
           -rtsp_transport udp -i "$URL"
    ;;
  tcp)
    ffplay -fflags nobuffer -flags low_delay \
           -rtsp_transport tcp -i "$URL"
    ;;
esac