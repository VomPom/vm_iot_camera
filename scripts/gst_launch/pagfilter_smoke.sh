#!/usr/bin/env bash
# pagfilter Stage 2 烟雾测试：用 videotestsrc 顶替摄像头，验证插件能加载、
# 链路能从 NULL → PLAYING、buffer 能透传到 fakesink，并演示 invert 属性。
#
# 前置条件：
#   - 已 cmake --build 出 vm_iot 主可执行文件（路径下方 BIN 变量）；
#   - 当前 shell 把 vm_iot 二进制拉起一次后，pagfilter 已注册到默认 registry。
#     但 gst-launch-1.0 不会加载 vm_iot 进程，因此默认情况下 gst-launch 看不到
#     pagfilter——这是项目当前"静态注册进 vm_iot 二进制"决策的副作用。
#
#   ⇒ 因此这份脚本默认走"在 vm_iot 进程内部"的等价命令：通过 vm_iot 自身的
#     pipeline_builder 启动一次最简管线即可，详见 README。本脚本仅作为
#     "future external .so" 落地后的占位。

set -euo pipefail

if ! command -v gst-launch-1.0 >/dev/null 2>&1; then
    echo "gst-launch-1.0 not found, skip" >&2
    exit 0
fi

# ── 用例 1：透传（Stage 1 行为，invert 默认 false） ──
# 如未来打成独立 .so 安装到 GST_PLUGIN_PATH，下面这行就能跑通：
gst-launch-1.0 -v \
    videotestsrc num-buffers=30 is-live=false \
    ! video/x-raw,format=I420,width=320,height=240,framerate=30/1 \
    ! pagfilter name=pag0 \
    ! fakesink sync=false

# ── 用例 2：颜色反相（Stage 2 新增） ──
# 启用 invert=true 后 pagfilter 取消 passthrough、走 transform_ip，
# 对每帧 I420 三个 plane 做 c = 255 - c。可以接 autovideosink 肉眼验证；
# CI 环境下用 fakesink，仅验证不崩 + 状态机能进 PLAYING。
gst-launch-1.0 -v \
    videotestsrc num-buffers=30 is-live=false pattern=smpte \
    ! video/x-raw,format=I420,width=320,height=240,framerate=30/1 \
    ! pagfilter name=pag0 invert=true \
    ! fakesink sync=false

# ── 用例 3（可视）：肉眼验证反相 ──
# 在桌面环境下取消注释即可看到反相画面（黑->白、白->黑、绿->紫……）：
# gst-launch-1.0 -v \
#     videotestsrc is-live=false pattern=smpte \
#     ! video/x-raw,format=I420,width=640,height=360,framerate=30/1 \
#     ! pagfilter invert=true \
#     ! videoconvert ! autovideosink
