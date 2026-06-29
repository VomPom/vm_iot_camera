//
// Created by vm_iot on 2026/06/29.
//
// @Description
//   见 audio_branch.h。
//

#include "audio_branch.h"

#include <gst/gst.h>

#include <cstdio>

#include "log.h"

void AudioBranch::configure(const AudioConfig& cfg) {
    cfg_ = cfg;
    LOGI("audio_branch: configured backend={} device='{}' rate={} ch={} "
         "codec={}/{} bitrate_kbps={} volume={:.3f} mute={}",
         to_string(cfg_.capture.backend),
         cfg_.capture.device.empty() ? "(default)" : cfg_.capture.device.c_str(),
         cfg_.capture.samplerate, cfg_.capture.channels,
         to_string(cfg_.encoder.backend), to_string(cfg_.encoder.aac_impl),
         cfg_.encoder.bitrate_kbps,
         cfg_.control.volume, cfg_.control.mute);
}

bool AudioBranch::on_attached_locked() {
    /* BranchBase 已经把 aud_vol / aud_valve 抓到了 elements_，且当前在 mu_
     * 保护下、元素还处于 NULL/READY，可安全注入属性。
     * volume 是 PLAYING-mutable，valve.drop 也是；这里只是把"启动期值"刷下去，
     * 后续 set_* 走同一份 g_object_set。 */
    GstElement* vol   = element("aud_vol");
    GstElement* valve = element("aud_valve");
    if (!vol || !valve) {
        LOGW("audio_branch: aud_vol/aud_valve missing in on_attached_locked");
        return false;
    }
    g_object_set(vol,   "volume", static_cast<gdouble>(cfg_.control.volume), nullptr);
    g_object_set(valve, "drop",   cfg_.control.mute ? TRUE : FALSE, nullptr);
    LOGI("audio_branch: attached, applied volume={:.3f} mute={}",
         cfg_.control.volume, cfg_.control.mute);
    return true;
}

bool AudioBranch::set_volume(float v, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* vol = element("aud_vol");
    if (!vol) { err = "not_attached"; return false; }
    /* clamp 与 yaml 校验同口径：[0, 10]。Pulse 模式以及未来 plugin 不影响。 */
    if (!(v >= 0.0f && v <= 10.0f)) {
        err = "invalid_volume";
        return false;
    }
    g_object_set(vol, "volume", static_cast<gdouble>(v), nullptr);
    LOGI("audio_branch: hot-set volume={:.3f}", v);
    return true;
}

bool AudioBranch::set_mute(bool m, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* valve = element("aud_valve");
    if (!valve) { err = "not_attached"; return false; }
    g_object_set(valve, "drop", m ? TRUE : FALSE, nullptr);
    LOGI("audio_branch: hot-set mute={}", m);
    return true;
}

void AudioBranch::format_status(std::string& out) const {
    /* 复用 mu_ 保证元素引用稳定；attached=false 时只输出最少字段。 */
    std::lock_guard<std::mutex> lk(mu_);
    GstElement* vol   = element("aud_vol");
    GstElement* valve = element("aud_valve");

    out.append("audio_attached=").append((vol && valve) ? "true" : "false").append("\n");
    out.append("audio_backend=" ).append(to_string(cfg_.capture.backend)).append("\n");
    out.append("audio_device="  ).append(cfg_.capture.device.empty() ? "default" : cfg_.capture.device).append("\n");
    out.append("audio_rate="    ).append(std::to_string(cfg_.capture.samplerate)).append("\n");
    out.append("audio_channels=").append(std::to_string(cfg_.capture.channels)).append("\n");
    out.append("audio_codec="   ).append(to_string(cfg_.encoder.backend));
    if (cfg_.encoder.backend == AudioEncoderBackend::AAC) {
        out.append("/").append(to_string(cfg_.encoder.aac_impl));
    }
    out.append("\n");
    out.append("audio_bitrate_kbps=").append(std::to_string(cfg_.encoder.bitrate_kbps)).append("\n");

    if (vol && valve) {
        gdouble cur_vol = 1.0;
        gboolean cur_drop = FALSE;
        g_object_get(vol,   "volume", &cur_vol,  nullptr);
        g_object_get(valve, "drop",   &cur_drop, nullptr);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", cur_vol);
        out.append("audio_volume=").append(buf).append("\n");
        out.append("audio_mute=").append(cur_drop ? "true" : "false").append("\n");
    } else {
        /* 未 attach 时输出启动期配置，便于运维确认 yaml 是否生效。 */
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", cfg_.control.volume);
        out.append("audio_volume=").append(buf).append("\n");
        out.append("audio_mute=").append(cfg_.control.mute ? "true" : "false").append("\n");
    }
}
