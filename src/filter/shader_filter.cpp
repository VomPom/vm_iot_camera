//
// Created by vompom on 2026/06/09.
//
// @Description
//

#include "shader_filter.h"
#include "config.h"
#include "log.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

/* 把整个文件读成 std::string；失败返回空串。 */
static std::string read_file_all(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return {};
    std::ostringstream os;
    os << ifs.rdbuf();
    return os.str();
}

void ShaderFilter::configure(const FilterConfig& filter_cfg,
                             const std::string& shaders_base_dir) {
    filter_cfg_       = &filter_cfg;
    shaders_base_dir_ = shaders_base_dir;
    /* 初始化 current_type_：以 yaml 指定值为准。
     * 真正的 fragment 文件加载延迟到 attach_to_media。 */
    if (filter_cfg.enabled) {
        std::lock_guard<std::mutex> lk(mu_);
        current_type_ = filter_cfg.filter_type;
    }
}

bool ShaderFilter::enabled() const {
    return filter_cfg_ && filter_cfg_->enabled;
}

int ShaderFilter::current_type() const {
    std::lock_guard<std::mutex> lk(mu_);
    return current_type_;
}

std::string ShaderFilter::resolve_shader_path() const {
    if (!filter_cfg_ || filter_cfg_->shader.empty()) return {};
    const std::string& name = filter_cfg_->shader;
    if (name.front() == '/') return name;
    return (std::filesystem::path(shaders_base_dir_) / name)
            .lexically_normal().string();
}

void ShaderFilter::push_uniforms(GstElement* shader, int type) const {
    if (!shader) return;

    /* GstGLShader 的 "uniforms" 属性接受一个 GstStructure，键名要和 shader 里
     * 声明的 uniform 完全一致。下一帧上传，无需重编译 GL Program。 */
    GstStructure* st = gst_structure_new("uniforms",
                                         "filter_type", G_TYPE_INT, type,
                                         nullptr);
    g_object_set(shader, "uniforms", st, nullptr);
    gst_structure_free(st);
}

void ShaderFilter::on_media_unprepared(GstRTSPMedia* media, gpointer user) {
    auto* self = static_cast<ShaderFilter*>(user);
    if (!self) return;

    GstElement* pipeline = gst_rtsp_media_get_element(media);
    if (!pipeline) return;
    GstElement* shader = gst_bin_get_by_name(GST_BIN(pipeline), "f0");
    gst_object_unref(pipeline);
    if (!shader) return;

    {
        std::lock_guard<std::mutex> lk(self->mu_);
        auto it = std::find(self->shaders_.begin(), self->shaders_.end(), shader);
        if (it != self->shaders_.end()) {
            gst_object_unref(*it);
            self->shaders_.erase(it);
            LOGI("media-unprepared: dropped one cached glshader, remain={}",
                 self->shaders_.size());
        }
    }
    /* by_name 又增加了一次 ref，无论是否在集合里都要 unref。 */
    gst_object_unref(shader);
}

void ShaderFilter::attach_to_media(GstRTSPMedia* media) {
    if (!enabled()) {
        LOGI("media-configure: filter disabled, skip shader injection");
        return;
    }

    GstElement* pipeline = gst_rtsp_media_get_element(media);
    if (!pipeline) {
        LOGW("media-configure: gst_rtsp_media_get_element returned null");
        return;
    }
    GstElement* shader = gst_bin_get_by_name(GST_BIN(pipeline), "f0");
    gst_object_unref(pipeline);
    if (!shader) {
        LOGW("media-configure: glshader 'f0' not found in pipeline");
        return;
    }

    /* 纳入热切换集合（持有 by_name 增加的那一份 ref）。 */
    {
        std::lock_guard<std::mutex> lk(mu_);
        shaders_.push_back(shader);
    }

    g_signal_connect(media, "unprepared",
                     G_CALLBACK(&ShaderFilter::on_media_unprepared), this);

    /* 注入 fragment 源码：优先用已缓存的，否则从文件加载一次。
     * 注：fragment 属性只在 caps 协商时编译，因此首次注入必须在 PLAYING 之前完成。 */
    std::string frag;
    int type = -1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!current_src_.empty()) {
            frag = current_src_;
            type = current_type_;
        }
    }
    if (frag.empty()) {
        std::string path = resolve_shader_path();
        frag = read_file_all(path);
        if (frag.empty()) {
            LOGW("media-configure: fragment file empty or unreadable: {}", path);
        } else {
            std::lock_guard<std::mutex> lk(mu_);
            current_src_ = frag;
            if (current_type_ < 0) current_type_ = filter_cfg_->filter_type;
            type = current_type_;
        }
    }

    if (!frag.empty()) {
        g_object_set(shader, "fragment", frag.c_str(), nullptr);
        LOGI("media-configure: injected fragment ({} bytes), filter_type={}",
             frag.size(), type);
    }

    /* 写入初始 uniform，保证 yaml 里配置的 filter_type 第一帧就生效。 */
    if (type >= 0) {
        push_uniforms(shader, type);
    }
}

void ShaderFilter::shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto* s : shaders_) gst_object_unref(s);
    shaders_.clear();
    current_src_.clear();
    current_type_ = -1;
}

/* ---------------- 热切换实现 ---------------- */

bool ShaderFilter::apply_type_locked(int type) {
    /* 调用方必须持有 mu_。 */
    if (!enabled()) {
        LOGW("apply_filter_type: filter disabled");
        return false;
    }
    if (type < 0 || type > filter_cfg_->max_type) {
        LOGW("apply_filter_type: type {} out of [0,{}]", type, filter_cfg_->max_type);
        return false;
    }

    /* 写入所有当前活跃的 glshader 元素：uniforms 在 PLAYING 状态下下一帧生效。 */
    for (auto* sh : shaders_) {
        push_uniforms(sh, type);
    }
    current_type_ = type;
    LOGI("filter switched: type={} active_media={}", type, shaders_.size());
    return true;
}

int ShaderFilter::next() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled()) return -1;
    int n = filter_cfg_->max_type + 1;       // [0, max_type] 共 n 个
    if (n <= 0) return -1;
    int next = (current_type_ < 0 ? 0 : (current_type_ + 1) % n);
    return apply_type_locked(next) ? current_type_ : -1;
}

int ShaderFilter::prev() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled()) return -1;
    int n = filter_cfg_->max_type + 1;
    if (n <= 0) return -1;
    int prev = (current_type_ <= 0 ? n - 1 : current_type_ - 1);
    return apply_type_locked(prev) ? current_type_ : -1;
}

bool ShaderFilter::set_type(int type) {
    std::lock_guard<std::mutex> lk(mu_);
    return apply_type_locked(type);
}

bool ShaderFilter::reload() {
    /* 仅重读 fragment 源码并重新注入到所有活跃 media。
     * 受 GstGLShader "fragment 仅在 caps 协商时编译" 限制：
     * PLAYING 中调用 reload，新源码不会立即编译，需重新建立连接才生效。
     * 这里只更新缓存与下次 attach_to_media 的注入内容。 */
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled()) return false;

    std::string path = resolve_shader_path();
    std::string src  = read_file_all(path);
    if (src.empty()) {
        LOGW("reload: empty/unreadable: {}", path);
        return false;
    }
    current_src_ = src;
    for (auto* sh : shaders_) {
        g_object_set(sh, "fragment", src.c_str(), nullptr);
    }
    /* 重新把当前 filter_type 推一次（不影响行为，仅保证 uniform 同步）。 */
    if (current_type_ >= 0) {
        for (auto* sh : shaders_) push_uniforms(sh, current_type_);
    }
    LOGI("shader reloaded: {} bytes ({} active media)", src.size(), shaders_.size());
    return true;
}