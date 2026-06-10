#!/usr/bin/env bash
# 录 H.264 与 H.265 各 30s, 在相同主观画质下比较码率
DUR=10
./vm_iot --set encoder.backend=x264
ffmpeg -nostdin -y -rtsp_transport tcp -i rtsp://127.0.0.1:8554/live -t $DUR -c copy h264.mp4

sleep 1

./vm_iot --set encoder.backend=x265
ffmpeg -nostdin -y -rtsp_transport tcp -i rtsp://127.0.0.1:8554/live -t $DUR -c copy h265.mp4


ls -l h264.mp4 h265.mp4
ffmpeg -i h264.mp4 -i h265.mp4 -lavfi psnr -f null - 2>&1 | tail -3