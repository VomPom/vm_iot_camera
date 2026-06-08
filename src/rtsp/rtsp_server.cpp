//
// Created by vompom on on 2026/06/05 17:08.
//
// @Description
//

#include "rtsp_server.h"
#include "pipeline_builder.h"
#include "log.h"
#include <string>


void RtspServer::on_client_connected(GstRTSPServer*, GstRTSPClient* c, gpointer) {
    GstRTSPConnection* conn = gst_rtsp_client_get_connection(c);
    const gchar* ip = gst_rtsp_connection_get_ip(conn);
    LOGI("client connected: {}", ip ? ip : "unknown");
}

bool RtspServer::start(const Config& cfg, PipelineBuilder::Mode mode) {
    server_ = gst_rtsp_server_new();
    if (!server_) {
        LOGE("gst_rtsp_server_new failed");
        return false;
    }

    std::string port_str = std::to_string(cfg.server.port);
    gst_rtsp_server_set_service(server_, port_str.c_str());

    mounts_  = gst_rtsp_server_get_mount_points(server_);
    factory_ = gst_rtsp_media_factory_new();

    std::string launch = PipelineBuilder::build(cfg, mode);
    LOGI("rtsp launch: {}", launch);
    gst_rtsp_media_factory_set_launch(factory_, launch.c_str());

    /* shared=TRUE: 多客户端共享同一份编码输出，CPU 不会随客户端数翻倍 */
    gst_rtsp_media_factory_set_shared(factory_, TRUE);

    /* 默认协议优先 UDP，TCP 兜底 */
    gst_rtsp_media_factory_set_protocols(factory_,
        (GstRTSPLowerTrans)(GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP));

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
}