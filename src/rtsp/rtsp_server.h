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

private:
    static void on_client_connected(GstRTSPServer* /*s*/, GstRTSPClient* c, gpointer user);

    /* 在 media 解析完 launch 字符串、但尚未进入 PLAYING 时回调，
     * 将事件转发给 ShaderFilter 完成 fragment 注入与 uniforms 写入。 */
    static void on_media_configure(GstRTSPMediaFactory* factory,
                                   GstRTSPMedia*        media,
                                   gpointer             user);

    GstRTSPServer*       server_  = nullptr;
    GstRTSPMountPoints*  mounts_  = nullptr;
    GstRTSPMediaFactory* factory_ = nullptr;
    guint                source_id_ = 0;

    ShaderFilter*        filter_ = nullptr;   // 不拥有，仅持指针
};

#endif //VM_IOT_RTSP_SERVER_H
