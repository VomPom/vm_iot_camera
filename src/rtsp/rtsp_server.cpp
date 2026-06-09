//
// Created by vompom on on 2026/06/05 17:08.
//
// @Description
//

#include "rtsp_server.h"
#include "pipeline_builder.h"
#include "log.h"
#include "config.h"
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>


/* 把整个文件读成 std::string；失败返回空串，回调里会记录日志后跳过 set。 */
static std::string read_file_all(const std::string &path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return {};
    std::ostringstream os;
    os << ifs.rdbuf();
    return os.str();
}

/* 路径拼接：如果 rel 是绝对路径直接返回，否则以 base 为基目录拼接。
 * base 为空时表示不能解析，原样返回 rel。 */
static std::string join_path(const std::string &base, const std::string &rel) {
    if (rel.empty()) return {};
    if (rel.front() == '/') return rel;
    if (base.empty()) return rel;
    return (std::filesystem::path(base) / rel).lexically_normal().string();
}

/* 解析 yaml 中的 shader 路径，优先级：
*/
static std::string resolve_shader_path(const Config &cfg) {
    const FilterConfig &f = cfg.filter;
    if (f.shader.empty()) return {};
    if (f.shader.front() == '/') return f.shader;

    std::string base = (std::filesystem::path(cfg.config_dir) / ".." / "shaders")
            .lexically_normal().string();
    return join_path(base, f.shader);
}


void RtspServer::on_client_connected(GstRTSPServer *, GstRTSPClient *c, gpointer) {
    GstRTSPConnection *conn = gst_rtsp_client_get_connection(c);
    const gchar *ip = gst_rtsp_connection_get_ip(conn);
    LOGI("client connected: {}", ip ? ip : "unknown");
}

void RtspServer::on_media_configure(GstRTSPMediaFactory * /*factory*/,
                                    GstRTSPMedia *media,
                                    gpointer user) {
    auto *self = static_cast<RtspServer *>(user);
    if (!self || !self->cfg_) {
        LOGW("media-configure: missing self/cfg");
        return;
    }

    /* filter 整段被禁用时，pipeline 中根本就没有 glshader，直接返回。 */
    if (!self->cfg_->filter.enabled) {
        LOGI("media-configure: filter disabled, skip shader injection");
        return;
    }

    /* 拿到本次 media 对应的 pipeline（GstBin），按内部约定名称 f0 找到 glshader。 */
    GstElement *pipeline = gst_rtsp_media_get_element(media);
    if (!pipeline) {
        LOGW("media-configure: gst_rtsp_media_get_element returned null");
        return;
    }

    GstElement *shader = gst_bin_get_by_name(GST_BIN(pipeline), "f0");
    if (!shader) {
        LOGW("media-configure: glshader 'f0' not found in pipeline");
        gst_object_unref(pipeline);
        return;
    }

    /* 读取 fragment 源码并注入。glshader 接受任意字符串，无需做任何转义。 */
    const std::string frag_path = resolve_shader_path(*self->cfg_);
    if (frag_path.empty()) {
        LOGW("media-configure: filter.shader is empty in config");
    } else {
        std::string frag = read_file_all(frag_path);
        if (frag.empty()) {
            LOGW("media-configure: fragment file empty or unreadable: {}", frag_path);
        } else {
            g_object_set(shader, "fragment", frag.c_str(), nullptr);
            LOGI("media-configure: injected fragment from {} ({} bytes)",
                 frag_path, frag.size());
        }
    }

    gst_object_unref(shader);
    gst_object_unref(pipeline);
}

bool RtspServer::start(const Config &cfg, PipelineBuilder::Mode mode) {
    cfg_ = &cfg;

    server_ = gst_rtsp_server_new();
    if (!server_) {
        LOGE("gst_rtsp_server_new failed");
        return false;
    }

    std::string port_str = std::to_string(cfg.server.port);
    gst_rtsp_server_set_service(server_, port_str.c_str());

    mounts_ = gst_rtsp_server_get_mount_points(server_);
    factory_ = gst_rtsp_media_factory_new();

    std::string launch = PipelineBuilder::build(cfg, mode);
    LOGI("rtsp launch: {}", launch);
    gst_rtsp_media_factory_set_launch(factory_, launch.c_str());

    /* shared=TRUE: 多客户端共享同一份编码输出，CPU 不会随客户端数翻倍 */
    gst_rtsp_media_factory_set_shared(factory_, TRUE);

    /* 默认协议优先 UDP，TCP 兜底 */
    gst_rtsp_media_factory_set_protocols(factory_,
                                         (GstRTSPLowerTrans) (GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP));

    /* 关键：在每次有新客户端拉流、media 被构造之后注入 fragment。
     * 该信号先于 PLAYING，能赶在 glshader 首次协商前生效。 */
    g_signal_connect(factory_, "media-configure",
                     G_CALLBACK(&RtspServer::on_media_configure), this);

    gst_rtsp_mount_points_add_factory(mounts_, cfg.server.mount.c_str(), factory_);
    /* add_factory 会持有 factory 的引用，下面要释放本地这一份 */
    g_object_unref(mounts_);
    mounts_ = nullptr;
    /* factory 同理: 用完释放本地引用，server 内部还持有一份 */

    g_signal_connect(server_, "client-connected",
                     G_CALLBACK(&RtspServer::on_client_connected), this);

    source_id_ = gst_rtsp_server_attach(server_, nullptr);
    if (source_id_ == 0) {
        LOGE("gst_rtsp_server_attach failed (port {} in use?)", cfg.server.port);
        return false;
    }

    LOGI("rtsp ready: rtsp://0.0.0.0:{}{}", cfg.server.port, cfg.server.mount);
    return true;
}

void RtspServer::stop() {
    if (source_id_) {
        g_source_remove(source_id_);
        source_id_ = 0;
    }
    if (server_) {
        g_object_unref(server_);
        server_ = nullptr;
    }
    cfg_ = nullptr;
}
