//
// Created by vompom on on 2026/06/05 17:08.
//
// @Description
//

#ifndef VM_IOT_RTSP_SERVER_H
#define VM_IOT_RTSP_SERVER_H

#include <gst/rtsp-server/rtsp-server.h>
#include "pipeline_builder.h"

struct Config;

class RtspServer {
public:
    bool start(const Config& cfg, PipelineBuilder::Mode mode);
    void stop();

private:
    static void on_client_connected(GstRTSPServer* /*s*/, GstRTSPClient* c, gpointer user);

    /* 在 media 解析完 launch 字符串、但尚未进入 PLAYING 时回调，
     * 这里向 glshader name=f1 注入 fragment 源码。 */
    static void on_media_configure(GstRTSPMediaFactory* factory,
                                   GstRTSPMedia*        media,
                                   gpointer             user);

    GstRTSPServer*       server_  = nullptr;
    GstRTSPMountPoints*  mounts_  = nullptr;
    GstRTSPMediaFactory* factory_ = nullptr;
    guint                source_id_ = 0;

    /* RtspServer 不拥有 cfg，仅持有指针；start() 期间外部 cfg 必须保持存活。 */
    const Config*        cfg_ = nullptr;
};

#endif //VM_IOT_RTSP_SERVER_H
