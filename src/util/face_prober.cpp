//
// Created by vompom on 2026/06/30.
//
// @Description
//   见 face_prober.h。实现策略：gst_element_factory_find + std::filesystem stat。
//

#include "face_prober.h"

#include <gst/gst.h>

#include <filesystem>
#include <sstream>
#include <system_error>

#include "log.h"

namespace face_prober {

namespace {

/* 检查单个 cascade xml 路径是否可读。
 * - 路径为空 → key_for_log 为空时报"cascade not found"，否则视为"可选未配置"返回 true。
 * - 路径不存在 / 不可读 → 失败 + 失败原因附 apt 提示。 */
bool cascade_readable(const std::string& path,
                      const char*        field_name,
                      bool               required,
                      std::string*       err) {
    if (path.empty()) {
        if (!required) return true;
        if (err) {
            std::ostringstream os;
            os << "face." << field_name << " is empty; "
               << "set it to a valid haarcascade xml (e.g. "
               << "/usr/share/opencv4/haarcascades/haarcascade_frontalface_default.xml). "
               << "If missing, install: sudo apt install libopencv-data";
            *err = os.str();
        }
        return false;
    }
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status) ||
        !std::filesystem::is_regular_file(status)) {
        if (err) {
            std::ostringstream os;
            os << "face." << field_name << " not a readable file: '" << path << "'";
            if (ec) os << " (" << ec.message() << ")";
            os << ". If missing, install: sudo apt install libopencv-data";
            *err = os.str();
        }
        return false;
    }
    return true;
}

} // namespace

bool is_ready(const FaceConfig& cfg, std::string* err) {
    if (!cfg.enabled) return true;

    /* 1) facedetect element factory 必须存在。 */
    GstElementFactory* f = gst_element_factory_find("facedetect");
    if (!f) {
        if (err) {
            *err = "GStreamer element 'facedetect' not found. "
                   "Install: sudo apt install gstreamer1.0-opencv";
        }
        return false;
    }
    gst_object_unref(f);

    /* 2) cascade 必填强校验；其它级联仅在非空时校验。 */
    if (!cascade_readable(cfg.detect.cascade, "detect.cascade", /*required*/true, err)) {
        return false;
    }
    if (!cascade_readable(cfg.detect.profile, "detect.profile", /*required*/false, err)) return false;
    if (!cascade_readable(cfg.detect.nose,    "detect.nose",    /*required*/false, err)) return false;
    if (!cascade_readable(cfg.detect.mouth,   "detect.mouth",   /*required*/false, err)) return false;
    if (!cascade_readable(cfg.detect.eyes,    "detect.eyes",    /*required*/false, err)) return false;

    LOGI("face_prober: ready (cascade='{}' fps_limit={})",
         cfg.detect.cascade, cfg.rate.fps_limit);
    return true;
}

} // namespace face_prober
