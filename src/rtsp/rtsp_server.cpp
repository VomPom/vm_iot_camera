//
// Created by vompom on on 2026/06/05 17:08.
//
// @Description
//   RTSP 服务器实现。仅承担 gst-rtsp-server 容器与媒体生命周期回调，
//   GL 滤镜逻辑已拆到 ShaderFilter。
//

#include "rtsp_server.h"
#include "pipeline_builder.h"
#include "shader_filter.h"
#include "log.h"
#include "config.h"

void RtspServer::on_client_connected(GstRTSPServer *, GstRTSPClient *c, gpointer) {
    GstRTSPConnection *conn = gst_rtsp_client_get_connection(c);
    const gchar *ip = gst_rtsp_connection_get_ip(conn);
    LOGI("client connected: {}", ip ? ip : "unknown");
}

void RtspServer::on_media_configure(GstRTSPMediaFactory * /*factory*/,
                                    GstRTSPMedia *media,
                                    gpointer user) {
    auto *self = static_cast<RtspServer *>(user);
    if (!self || !self->filter_) return;
    self->filter_->attach_to_media(media);
}

bool RtspServer::start(const Config &cfg, ShaderFilter *filter) {
    filter_ = filter;

    server_ = gst_rtsp_server_new();
    if (!server_) {
        LOGE("gst_rtsp_server_new failed");
        return false;
    }

    std::string port_str = std::to_string(cfg.server.port);
    gst_rtsp_server_set_service(server_, port_str.c_str());

    mounts_ = gst_rtsp_server_get_mount_points(server_);
    factory_ = gst_rtsp_media_factory_new();

    std::string launch = PipelineBuilder::build(cfg);
    LOGI("rtsp launch: {}", launch);
    gst_rtsp_media_factory_set_launch(factory_, launch.c_str());

    gst_rtsp_media_factory_set_shared(factory_, TRUE);

    gst_rtsp_media_factory_set_protocols(factory_,
                                         (GstRTSPLowerTrans)(GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP));

    g_signal_connect(factory_, "media-configure",
                     G_CALLBACK(&RtspServer::on_media_configure), this);

    gst_rtsp_mount_points_add_factory(mounts_, cfg.server.mount.c_str(), factory_);
    g_object_unref(mounts_);
    mounts_ = nullptr;

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
    filter_ = nullptr;
}