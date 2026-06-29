//
// Created by vm_iot on 2026/06/29.
//
// @Description
//   ALSA 设备可用性快速探测器。仅暴露一个对偶 v4l2_prober::device_accessible
//   的接口，启动期失败给出与 v4l2 同风格的人类可读错误并退出。
//
//   实现策略（三档退化）：
//     1) 编译期 VM_IOT_HAVE_ALSA_LIB=1：snd_pcm_open(name, CAPTURE, NONBLOCK) →
//        成功立即 close。覆盖"设备不存在 / 权限不够 / 被占用"三种情况。
//     2) 编译期 VM_IOT_HAVE_ALSA_LIB=0 但运行在 Linux：读 /proc/asound/cards
//        非空 + device 串非空，弱检查；不能区分占用/权限。
//     3) 非 Linux：永远 true，让调用方跳过检查（mac 端 build/CI 不误报）。
//
//   未来若要做完整能力探测（采样率 / 通道 / 格式），再开 AlsaProber 类。
//   当前最小实现，能在启动期把"麦克风没插"挡掉即可。
//

#ifndef VM_IOT_ALSA_PROBER_H
#define VM_IOT_ALSA_PROBER_H

#include <string>

namespace alsa_prober {

/**
 * 快速检测 ALSA 设备是否存在且可读打开。用于启动期前置检查：
 * 设备没插 / 权限不够 / 被独占时直接给出人类可读错误，而非等到 RTSP 客户端
 * 连上来才看到一句模糊的 GStreamer 错误。
 *
 * @param device alsasrc 的 device 字符串，例如 "hw:0,0" / "plughw:0,0"。
 *               空串视为"系统默认"，统一返回 true 让 alsasrc 自己挑。
 * @return true=可访问 / 非 Linux 兜底；false=确实不可访问。
 */
bool device_accessible(const std::string& device);

} // namespace alsa_prober

#endif // VM_IOT_ALSA_PROBER_H
