//
// Created by vompom on on 2026/06/05 17:08.
//
// @Description
//   RTSP 服务器实现。仅承担 gst-rtsp-server 容器与媒体生命周期回调，
//
//   监听链路（按时序）：
//     server                    media                    pipeline-bus
//     ──────                    ─────                    ────────────
//     client-connected ─┐
//                       │   media-configure ─┐
//                       │                    │  attach ShaderFilter
//                       │                    │  add bus watch ──► on_bus_message
//                       │                    │      ├─ ERROR     (log)
//                       │                    │      ├─ WARNING   (log)
//                       │                    │      ├─ EOS       (log)
//                       │                    │      └─ STATE_CHG (仅 pipeline 自己, log)
//                       │   media-prepared    (log)
//                       │   ...
//                       │   media-unprepared  (log + ShaderFilter 清理)
//     client-closed     (log)
//

#include "rtsp_server.h"
#include "pipeline_builder.h"
#include "shader_filter.h"
#include "log.h"
#include "config.h"

/* 取 client peer ip，统一一处避免到处写。 */
static const gchar* client_ip(GstRTSPClient* c) {
    if (!c) return "unknown";
    GstRTSPConnection* conn = gst_rtsp_client_get_connection(c);
    const gchar* ip = conn ? gst_rtsp_connection_get_ip(conn) : nullptr;
    return ip ? ip : "unknown";
}

void RtspServer::on_client_connected(GstRTSPServer*, GstRTSPClient* c, gpointer user) {
    LOGI("client connected: {}", client_ip(c));
    /* 客户端断开信号是挂在 client 对象上的。 */
    g_signal_connect(c, "closed", G_CALLBACK(&RtspServer::on_client_closed), user);
}

void RtspServer::on_client_closed(GstRTSPClient* c, gpointer /*user*/) {
    LOGI("client closed: {}", client_ip(c));
}

gboolean RtspServer::on_bus_message(GstBus* /*bus*/, GstMessage* msg, gpointer /*user*/) {
    /* 在默认 GMainContext 线程被调用，可安全调用 LOG*。 */
    const gchar* src_name = GST_OBJECT_NAME(GST_MESSAGE_SRC(msg));
    if (!src_name) src_name = "(null)";

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar*  dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            LOGE("pipeline error: src={} domain={} code={} message=\"{}\"",
                 src_name,
                 err ? g_quark_to_string(err->domain) : "?",
                 err ? err->code : -1,
                 err && err->message ? err->message : "(null)");
            if (dbg && *dbg) {
                /* debug 字符串可能很长，单独一行避免污染主日志。 */
                LOGE("pipeline error debug: {}", dbg);
            }
            if (err) g_error_free(err);
            if (dbg) g_free(dbg);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* err = nullptr;
            gchar*  dbg = nullptr;
            gst_message_parse_warning(msg, &err, &dbg);
            LOGW("pipeline warning: src={} message=\"{}\"",
                 src_name,
                 err && err->message ? err->message : "(null)");
            if (dbg && *dbg) {
                LOGW("pipeline warning debug: {}", dbg);
            }
            if (err) g_error_free(err);
            if (dbg) g_free(dbg);
            break;
        }
        case GST_MESSAGE_EOS: {
            /* 直播链路里通常不该出现，出现一般意味着上游被关闭。 */
            LOGI("pipeline EOS from {}", src_name);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            /* 只关心 pipeline 自身状态切换，避免每个 element 都打一行噪音。 */
            GstObject* msg_src = GST_MESSAGE_SRC(msg);
            if (msg_src && GST_IS_PIPELINE(msg_src)) {
                GstState old_st, new_st, pending;
                gst_message_parse_state_changed(msg, &old_st, &new_st, &pending);
                LOGI("pipeline state: {} -> {} (pending={})",
                     gst_element_state_get_name(old_st),
                     gst_element_state_get_name(new_st),
                     gst_element_state_get_name(pending));
            }
            break;
        }
        default:
            break;
    }
    return TRUE;   // 继续监听
}

void RtspServer::on_media_configure(GstRTSPMediaFactory* /*factory*/,
                                    GstRTSPMedia* media,
                                    gpointer user) {
    auto* self = static_cast<RtspServer*>(user);
    if (!self) return;

    /* 1) 交给 ShaderFilter 注入 fragment / uniforms。 */
    if (self->filter_) {
        self->filter_->attach_to_media(media);
    }

    /* 2) 挂 bus watch：拿到 media 的内部 pipeline → 它的 bus → add_watch。
     *    pipeline / bus 销毁时 watch 自动失效，无需保存 source_id。 */
    GstElement* pipeline = gst_rtsp_media_get_element(media);
    if (pipeline) {
        GstBus* bus = gst_element_get_bus(pipeline);
        if (bus) {
            gst_bus_add_watch(bus, &RtspServer::on_bus_message, self);
            gst_object_unref(bus);
        }
        gst_object_unref(pipeline);
    } else {
        LOGW("media-configure: get_element returned null, bus watch skipped");
    }

    /* 3) media 生命周期观测信号。 */
    g_signal_connect(media, "prepared",
                     G_CALLBACK(&RtspServer::on_media_prepared), self);
    g_signal_connect(media, "unprepared",
                     G_CALLBACK(&RtspServer::on_media_unprepared), self);
}

void RtspServer::on_media_prepared(GstRTSPMedia* /*media*/, gpointer /*user*/) {
    LOGI("media prepared (ready to stream)");
}

void RtspServer::on_media_unprepared(GstRTSPMedia* /*media*/, gpointer /*user*/) {
    /* 注：ShaderFilter 自己也接了 unprepared 信号清理 shaders_，互不影响。 */
    LOGI("media unprepared (released)");
}

bool RtspServer::start(const Config& cfg, ShaderFilter* filter) {
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

int RtspServer::client_count() const {
    if (!server_) return 0;
    /* gst_rtsp_server_client_filter 传 NULL filter 等价于"取全部 client"；
     * 返回的 GList 中每个 client 已被 ref，需用 g_object_unref 清理。 */
    GList* list = gst_rtsp_server_client_filter(server_, nullptr, nullptr);
    int n = static_cast<int>(g_list_length(list));
    g_list_free_full(list, g_object_unref);
    return n;
}