//
// Created by vompom  on 2026/06/05 17:08.
//
// @Description
//   RTSP 服务器实现。承担 gst-rtsp-server 容器与媒体生命周期回调。
//
//   监听链路（按时序）：
//     server                    media                    pipeline-bus
//     ──────                    ─────                    ────────────
//     client-connected ─┐
//                       │   media-configure ─┐
//                       │                    │  attach ShaderFilter
//                       │                    │  gst_bus_enable_sync_message_emission(bus)
//                       │                    │  g_signal_connect(bus, "sync-message", s_on_sync_message)
//                       │                    │      ├─ ERROR / WARNING / EOS / STATE_CHG (log)
//                       │                    │      └─ 广播给每个 branch→BranchBase::on_bus_message_sync
//                       │                    │  attach branches (face / record ...)
//                       │   media-prepared    (log)
//                       │   ...
//                       │   media-unprepared  (disconnect + disable_sync_emit + ShaderFilter 清理)
//     client-closed     (log)
//
//   Bus 订阅方式选择 gst_bus_enable_sync_message_emission +
//   g_signal_connect("sync-message")，不用 set_sync_handler / add_signal_watch：
//     - watch 类 API 依赖 GMainContext 派发，而 media-configure 在 rtsp-server
//       工作线程回调，该线程无 loop 在跑；主线程 default context 又被 main.cpp
//       的 g_main_loop_run 占用，无法 push_thread_default，两边都不通。
//     - set_sync_handler 独占 bus 的 sync handler slot，会干扰 rtsp-server
//       internal preroll 时序，出现"bus 消息到但画面推不出"。
//     - enable_sync_message_emission 是引用计数式多订阅 API，不抢 slot；
//       signal 在 streaming 线程同步 emit，无需 GMainContext。
//     - 代价：回调在热路径，不能阻塞（当前只做 LOG + parse + 短临界区）。
//
//   ★ 关键坑：必须订阅"外层 GstPipeline 的 bus"，不是
//     gst_rtsp_media_get_element(media) 返回的 GstBin 的 bus。
//     后者是 factory launch 出的 element bin，被 gst_bin_add 挂到 pipeline 后
//     其 bus 被替换为 pipeline 的 child_bus——child_bus 的 sync handler 直接
//     GST_BUS_DROP，signal 装了也 0 触发。真正外部 bus 需通过
//     gst_pipeline_get_bus 从 GstPipeline 上取。
//

#include "rtsp_server.h"
#include "pipeline_builder.h"
#include "shader_filter.h"
#include "branch_base.h"
#include "log.h"
#include "config.h"
#include "launch_pretty.h"

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
    /* 【历史遗留】此回调仅保留作为公共日志处理体，供 s_bus_sync_handler
     * 直接调用（在 streaming 线程）。若未来重新启用 add_signal_watch 方式，
     * 本回调仍兼容（返 G_SOURCE_CONTINUE）。 */
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
    /* signal_watch 重新回归后本回调作为 g_signal_connect("message") 的处理器，
     * 返回值会被忽略。返 TRUE 在 GstBusFunc 语义下也代表保持 watch。 */
    return G_SOURCE_CONTINUE;
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

    /* 2) 启用 sync-message signal emission（GStreamer 官方为"多订阅者同步获
     *    取消息"专门设计的 API，引用计数式，不影响 bus 的 sync handler slot，
     *    因此不会干扰 rtsp-server 内部 preroll。g_signal_connect("sync-message")
     *    可以多个共存，本项目采取“RtspServer 独占 + 内部广播给 branch”方式，
     *    避免多路径干扰。sync-message 在发送方的 streaming 线程同步 emit，
     *    回调里仅做 LOG + parse + 短临界区 mutex，不重不阻塞。 */
    if (self->bus_ || self->bus_sync_emit_enabled_ || self->bus_handler_id_) {
        /* 理论上不会：unprepared 时已置空。若出现说明存在多 media 场景，
         * 需要改造成 map<GstRTSPMedia*, ...>。这里先 warn + 主动清理，避免泄漏。 */
        LOGW("media-configure: stale bus_={} sync_emit={} handler={} detected, cleaning up",
             (void*)self->bus_,
             self->bus_sync_emit_enabled_ ? 1 : 0,
             (unsigned long)self->bus_handler_id_);
        if (self->bus_ && self->bus_handler_id_) {
            g_signal_handler_disconnect(self->bus_, self->bus_handler_id_);
            self->bus_handler_id_ = 0;
        }
        if (self->bus_ && self->bus_sync_emit_enabled_) {
            gst_bus_disable_sync_message_emission(self->bus_);
            self->bus_sync_emit_enabled_ = false;
        }
        if (self->bus_) {
            gst_object_unref(self->bus_);
            self->bus_ = nullptr;
        }
    }

    /* ★★ 关键：gst_rtsp_media_get_element 返回的是 factory launch 拼出来的 GstBin
     *    （priv->element），不是 rtsp-server 外层的 GstPipeline（priv->pipeline）。
     *    在 take_pipeline 时 rtsp-server 会 gst_bin_add(priv->pipeline, priv->element)，
     *    而 gst_bin_add 内部会执行 gst_element_set_bus(element, bin->child_bus) —— 
     *    也就是把 priv->element 的 bus 设成 priv->pipeline 的 child_bus。
     *    child_bus 是 GstBin 用来监听 child 消息的内部 bus，装了 bin_bus_handler 作为
     *    sync handler，且 enable-async=FALSE 无 poll，sync handler 返回 GST_BUS_DROP，
     *    消息 unref 后不会走 emit_sync_message 分支，也不会 push 到 async queue。
     *    因此若在这里写 gst_element_get_bus(pipeline_bin) 拿到的 bus 上订阅 sync-message，
     *    永远不会触发（这就是之前"signal 装了却 0 触发"的根因）。
     *
     *    真正的外部 bus 需要沿 parent 链向上找到 GstPipeline，再走 gst_pipeline_get_bus。
     *    rtsp-media 的 bus_message 就是这么装的（rtsp-media.c: gst_pipeline_get_bus(priv->pipeline)）。
     *    只有外部 bus 才没有 sync handler，enable-async=TRUE 有 poll，sync-message 才能 emit。 */
    GstElement* pipeline_bin = gst_rtsp_media_get_element(media);
    GstElement* toplevel     = nullptr;
    if (pipeline_bin) {
        /* 沿 parent 链向上找 GST_IS_PIPELINE 的祖先。通常一层就到，但为了健壮，走循环。 */
        toplevel = pipeline_bin;
        gst_object_ref(toplevel);           // 平衡后面的 unref
        while (toplevel && !GST_IS_PIPELINE(toplevel)) {
            GstObject* parent = gst_element_get_parent(toplevel);
            gst_object_unref(toplevel);     // 释放上一层
            toplevel = GST_ELEMENT_CAST(parent);
        }
    }
    if (toplevel && GST_IS_PIPELINE(toplevel)) {
        GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE_CAST(toplevel));
        if (bus) {
            gst_bus_enable_sync_message_emission(bus);
            self->bus_sync_emit_enabled_ = true;
            /* "sync-message" 信号支持 detail，不写 detail 则收所有类型。 */
            self->bus_handler_id_ = g_signal_connect(
                bus, "sync-message",
                G_CALLBACK(&RtspServer::s_on_sync_message), self);
            self->bus_ = bus;                       // 保留 ref，unprepared 时释放
            LOGI("media-configure: sync-message signal connected on pipeline bus "
                 "(pipeline={} bus={} handler={})",
                 (void*)toplevel, (void*)bus,
                 (unsigned long)self->bus_handler_id_);
        } else {
            LOGW("media-configure: gst_pipeline_get_bus returned null, sync-message skipped");
        }
    } else {
        LOGW("media-configure: could not locate GstPipeline ancestor, sync-message skipped "
             "(pipeline_bin={} toplevel={})",
             (void*)pipeline_bin, (void*)toplevel);
    }
    if (toplevel) gst_object_unref(toplevel);
    if (pipeline_bin) gst_object_unref(pipeline_bin);

    /* 3) 顺序 attach 所有 branches（snapshot / record / face / 未来的 detect ...）。
     *    branch 不再自己订阅 bus，其 on_bus_message_sync 会在 s_on_sync_message 里被广播。 */
    for (auto* b : self->branches_) {
        if (b) b->attach_to_media(media);
    }

    /* 4) media 生命周期观测信号。 */
    g_signal_connect(media, "prepared",
                     G_CALLBACK(&RtspServer::on_media_prepared), self);
    g_signal_connect(media, "unprepared",
                     G_CALLBACK(&RtspServer::on_media_unprepared), self);
}

void RtspServer::on_media_prepared(GstRTSPMedia* /*media*/, gpointer /*user*/) {
    LOGI("media prepared (ready to stream)");
}

void RtspServer::on_media_unprepared(GstRTSPMedia* /*media*/, gpointer user) {
    auto* self = static_cast<RtspServer*>(user);
    /* 注：ShaderFilter / 各 Branch 也各自接了 unprepared 或 detach 清理，互不影响。
     * 顺序：先 disconnect signal handler（避免后续往已销毁的 self 投递回调），
     * 再 disable_sync_message_emission（引用计数 -1，对应前面的 enable），
     * 最后 unref bus。 */
    if (self) {
        if (self->bus_ && self->bus_handler_id_) {
            g_signal_handler_disconnect(self->bus_, self->bus_handler_id_);
            self->bus_handler_id_ = 0;
        }
        if (self->bus_ && self->bus_sync_emit_enabled_) {
            gst_bus_disable_sync_message_emission(self->bus_);
            self->bus_sync_emit_enabled_ = false;
        }
        if (self->bus_) {
            gst_object_unref(self->bus_);
            self->bus_ = nullptr;
        }
    }
    LOGI("media unprepared (released)");
}

/* ---- sync-message signal handler ----
 * 在 GStreamer 发送消息的 streaming 线程同步 emit（与 sync handler 同一线程语义）。
 * 与 set_sync_handler 的区别：不会占用 bus 的 sync handler slot，rtsp-server 内部
 * 预期的默认 sync 行为（消息 push 至 async queue 等）不受影响，preroll、rtpbin
 * 信号传递均保持原样（避免了"消息到了但推不出画面"的副作用）。
 *
 * 步骤：
 *   1) 直接复用 on_bus_message 处理 log 类消息（ERROR/WARNING/EOS/STATE_CHG）；
 *   2) 广播给所有 branch→BranchBase::on_bus_message_sync。
 *
 * signal callback 本身无返回值（sync-message 不介意处理后续行为），
 * 消息仍按 bus 默认行为继续递交。
 *
 * 禁忌：此回调位于热路径，不得阻塞、不得重业务、不得 unref message。 */
void RtspServer::s_on_sync_message(GstBus* bus, GstMessage* msg, gpointer user) {
    auto* self = static_cast<RtspServer*>(user);
    if (!self) return;
    /* 1) RtspServer 自己的日志处理（复用已有的 on_bus_message）。 */
    RtspServer::on_bus_message(bus, msg, self);
    /* 2) 广播给 branches。branch 内部 override on_bus_message_sync 自行隔离消息。 */
    for (auto* b : self->branches_) {
        if (b) b->on_bus_message_sync(msg);
    }
}

bool RtspServer::start(const Config& cfg,
                       ShaderFilter* filter,
                       std::vector<BranchBase*> branches) {
    filter_   = filter;
    branches_ = std::move(branches);

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
    /* 调试可读形态：分组缩进，便于一眼看出主线/各副线拓扑。
     * 单行原始串也保留，方便复制粘贴到 gst-launch-1.0 复现问题。 */
    LOGI("rtsp pipeline:\n{}", launch_pretty::render(launch));
    LOGI("rtsp launch (raw): {}", launch);
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
    filter_   = nullptr;
    branches_.clear();
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