//
// Created by vompom on on 2026/06/05 17:08.
//
// @Description
//   RTSP 服务器封装：仅负责 gst-rtsp-server 的生命周期与媒体生命周期回调。
//   GL 滤镜（fragment 注入 / uniform 切换）已拆分到 ShaderFilter。
//

#ifndef VM_IOT_RTSP_SERVER_H
#define VM_IOT_RTSP_SERVER_H

#include <gst/rtsp-server/rtsp-server.h>
#include "pipeline_builder.h"

struct Config;
class ShaderFilter;

class RtspServer {
public:
    /* filter 可为 nullptr：表示 pipeline 不带 GL 滤镜段。
     * filter 必须在 RtspServer 生命周期内保持存活，本类不拥有它。 */
    bool start(const Config& cfg, ShaderFilter* filter);
    void stop();

    /* 当前 RTSP 客户端连接数（线程安全：内部走 gst_rtsp_server_client_filter）。
     * 未 start 或已 stop 时返回 0。 */
    int client_count() const;

private:
    /* ---- server 级信号 ---- */
    static void on_client_connected(GstRTSPServer* /*s*/, GstRTSPClient* c, gpointer user);
    static void on_client_closed(GstRTSPClient* c, gpointer user);

    /* ---- media 级信号 ----
     * media-configure：在 launch 解析完、未 PLAYING 前回调；
     *                  我们在这里 (1) 转发给 ShaderFilter 注入 fragment；
     *                  (2) 拿到内部 pipeline 的 GstBus 挂上 watch；
     *                  (3) 监听 prepared / unprepared 用于观测 media 生命周期。 */
    static void on_media_configure(GstRTSPMediaFactory* factory,
                                   GstRTSPMedia*        media,
                                   gpointer             user);
    static void on_media_prepared(GstRTSPMedia* media, gpointer user);
    static void on_media_unprepared(GstRTSPMedia* media, gpointer user);

    /* ---- pipeline bus watch ----
     * 在 GLib 主线程异步回调，仅做日志记录，不主动 unprepare。
     * 返回 TRUE 表示继续监听；FALSE 表示移除。 */
    static gboolean on_bus_message(GstBus* bus, GstMessage* msg, gpointer user);

    GstRTSPServer*       server_  = nullptr;
    GstRTSPMountPoints*  mounts_  = nullptr;
    GstRTSPMediaFactory* factory_ = nullptr;
    guint                source_id_ = 0;

    ShaderFilter*        filter_ = nullptr;   // 不拥有，仅持指针
};

#endif //VM_IOT_RTSP_SERVER_H
