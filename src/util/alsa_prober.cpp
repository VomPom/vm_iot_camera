//
// Created by vm_iot on 2026/06/29.
//
// @Description
//   见 alsa_prober.h。三档退化实现。
//

#include "alsa_prober.h"

#include "log.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>

#if VM_IOT_HAVE_ALSA_LIB
extern "C" {
#include <alsa/asoundlib.h>
}
#endif

namespace alsa_prober {

#if VM_IOT_HAVE_ALSA_LIB

bool device_accessible(const std::string& device) {
    /* 空串：让 alsasrc 自己挑系统默认设备，不走 prober。 */
    if (device.empty()) return true;

    snd_pcm_t* pcm = nullptr;
    int rc = snd_pcm_open(&pcm,
                          device.c_str(),
                          SND_PCM_STREAM_CAPTURE,
                          SND_PCM_NONBLOCK);
    if (rc < 0) {
        /* 把 ALSA 的错误码映射到人类可读串。常见值：
         *   -ENOENT(2)  设备不存在
         *   -EACCES(13) 权限不够（usermod -aG audio）
         *   -EBUSY(16)  设备被独占
         *   -ENODEV(19) 内核无相应模块 */
        LOGW("alsa_prober: snd_pcm_open('{}') failed: {}",
             device, snd_strerror(rc));
        return false;
    }
    snd_pcm_close(pcm);
    return true;
}

#elif defined(__linux__)

/* 没装 alsa-lib：退化为读 /proc/asound/cards 是否非空。
 * 不能精确区分"设备不存在 / 被占 / 权限"，只能在"完全没声卡"时返 false。
 * 启动期日志里附带一行 hint，便于排查。 */
bool device_accessible(const std::string& device) {
    if (device.empty()) return true;

    std::ifstream f("/proc/asound/cards");
    if (!f) {
        LOGW("alsa_prober: /proc/asound/cards not readable, assuming inaccessible");
        return false;
    }
    std::string line;
    bool any = false;
    while (std::getline(f, line)) {
        /* 内核里 "no soundcards" 行长度固定，简单匹配即可 */
        if (line.find("no soundcards") != std::string::npos) {
            any = false;
            break;
        }
        if (!line.empty() && line[0] >= '0' && line[0] <= '9') {
            any = true;
        }
    }
    if (!any) {
        LOGW("alsa_prober: no soundcards in /proc/asound/cards (alsa-lib not built in, "
             "weak check). device='{}'", device);
    } else {
        LOGI("alsa_prober: alsa-lib not built in, weak check passed; "
             "device='{}' availability not strictly verified", device);
    }
    return any;
}

#else // 非 Linux 平台

bool device_accessible(const std::string& /*device*/) {
    /* macOS / 其他平台：build/CI 友好兜底，不误报。 */
    return true;
}

#endif

} // namespace alsa_prober
