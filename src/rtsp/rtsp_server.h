//
// Created by vompom on on 2026/06/05 17:08.
//
// @Description
//

#ifndef VM_IOT_RTSP_SERVER_H
#define VM_IOT_RTSP_SERVER_H

#include <gst/rtsp-server/rtsp-server.h>
struct Config;

class RtspServer {
public:
    bool start(const Config& cfg);
    void stop();

private:
    static void on_client_connected(GstRTSPServer* /*s*/, GstRTSPClient* c, gpointer user);

    GstRTSPServer*       server_  = nullptr;
    GstRTSPMountPoints*  mounts_  = nullptr;
    GstRTSPMediaFactory* factory_ = nullptr;
    guint                source_id_ = 0;
};

#endif //VM_IOT_RTSP_SERVER_H
