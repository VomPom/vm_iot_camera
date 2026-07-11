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
#include <atomic>
#include <mutex>
#include <vector>
#include "pipeline_builder.h"

struct Config;
class ShaderFilter;
class BranchBase;

class RtspServer {
public:
    /* filter 可为 nullptr：表示 pipeline 不带 GL 滤镜段。
     * branches 列表里的每个对象在 media-configure 时会被依次 attach；
     * 它们与 filter 一样必须在 RtspServer 生命周期内保持存活，本类不拥有。
     * 通过基类 BranchBase 多态分发，未来新增 detect / cloud_upload 等副线
     * 只需 push 到该列表，无需修改 RtspServer。 */
    bool start(const Config& cfg,
               ShaderFilter* filter,
               std::vector<BranchBase*> branches);
    void stop();

    /* 当前 RTSP 客户端连接数（线程安全：内部走 gst_rtsp_server_client_filter）。
     * 未 start 或已 stop 时返回 0。 */
    int client_count() const;

    /**
     * 主动让当前 media 进入 unprepared 状态，触发 gst-rtsp-server 断开所有
     * client 并释放 pipeline；下一次 client DESCRIBE 会重新走 media-configure
     * → build_source_segment → v4l2src open，从而实现"设备热插拔后自动重连"。
     *
     * 调用者：
     *   - s_on_sync_message 内部：识别到 v4l2src 报 GST_RESOURCE_ERROR 时。
     *
     * 线程安全：本方法可从任意线程调用，内部会用 g_idle_add 把真正的
     * gst_rtsp_media_unprepare 转到 GMainLoop 线程执行——rtsp-server 的 media
     * 状态机不允许在 streaming 线程直接 unprepare（会死锁）。
     *
     * @param reason 记录到日志的原因串，便于事后 grep 分辨触发源
     *               ("bus_error" 等)。
     * @return true=已成功 post 一个 unprepare 任务；
     *         false=当前没有活跃 media（例如 client 已断开、pipeline 已释放）。
     *         注意返回 true 只代表"任务已入队"，不代表 unprepare 已完成。
     */
    bool force_unprepare_current_media(const char* reason);

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

    /* ---- pipeline bus 处理【sync-message signal 模式】----
     * 使用 gst_bus_enable_sync_message_emission + "sync-message" signal
     * 而不使用 gst_bus_set_sync_handler，也不使用 add_watch / add_signal_watch。
     * 背景（逆向精选）：
     *   1) media-configure / on_attached_locked 由 gst-rtsp-server 在 client 工作
     *      线程回调，add_watch/add_signal_watch 内部 GSource 会被 attach 到
     *      "调用时线程的 thread-default GMainContext"；那个 context 上没有 loop
     *      在跑 → 回调永不触发。而主线程 default context 已被 main.cpp
     *      g_main_loop_run acquire，工作线程 push_thread_default 它会 assert。
     *   2) gst_bus_set_sync_handler 将 slot 全部占住，会覆盖 rtsp-server / bin
     *      内部可能在未来版本依赖的 sync handler 行为（官方文档：“usually
     *      only called by the creator of the bus”）。实际上也确实存在副作用：
     *      pipeline preroll 因 sync handler PASS 取代默认行为后，rtpbin/videosink
     *      的 STATE_CHANGED / ASYNC_DONE 派发时序发生变化，导致 rtsp-server
     *      内部 async watch 拿不到预期中的 preroll 完成信号 → 推不出画面。
     *   3) gst_bus_enable_sync_message_emission 是 GStreamer 官方为“多订阅者
     *      同步获取消息”专门设计的 API，引用计数式、不影响 bus 自己的
     *      sync handler slot，多个 g_signal_connect("sync-message") 可以共存。
     *   4) sync-message signal 在发送方的 streaming 线程同步 emit，与 sync
     *      handler 同一线程语义，处理时仍需严遵“不阻塞、不重业务”。
     *   5) 一个 bus 可以多个 signal handler 同时存在，本服务器 + 各 branch
     *      都可以各自 g_signal_connect（本项目采取 RtspServer 独占、内部广播
     *      的方式避免多路径干扰）。 */
    static gboolean on_bus_message(GstBus* bus, GstMessage* msg, gpointer user);
    static void     s_on_sync_message(GstBus* bus, GstMessage* msg, gpointer user);

    /* g_idle_add 的 trampoline：在 GMainLoop 线程执行 gst_rtsp_media_unprepare。
     * user_data 是堆分配的 UnprepareTask*，回调结束时释放。返回 G_SOURCE_REMOVE
     * 保证一次性执行。 */
    static gboolean s_do_unprepare_on_main(gpointer user);

    GstRTSPServer*       server_  = nullptr;
    GstRTSPMountPoints*  mounts_  = nullptr;
    GstRTSPMediaFactory* factory_ = nullptr;
    guint                source_id_ = 0;

    ShaderFilter*            filter_   = nullptr;   // 不拥有，仅持指针
    std::vector<BranchBase*> branches_;             // 不拥有，仅持指针

    /* pipeline bus 与 sync-message signal 的生命周期：
     *   media-configure   : gst_bus_enable_sync_message_emission(bus)
     *                       + g_signal_connect(bus, "sync-message", ...)
     *   media-unprepared  : g_signal_handler_disconnect + 
     *                       gst_bus_disable_sync_message_emission(bus)
     *                       + gst_object_unref(bus)
     * enable/disable 严格成对回收引用计数，避免影响其他订阅者。 */
    GstBus*             bus_                     = nullptr;   // 持 ref
    gulong              bus_handler_id_          = 0;         // g_signal_connect 返回值
    bool                bus_sync_emit_enabled_   = false;

    /* 当前活跃 media（media-configure 记录；media-unprepared 清空）。
     * 用于 force_unprepare_current_media 与 sync-message 里对 media 的引用。
     * 用 mutex 保护是因为 media-configure 在 rtsp-server 工作线程写，
     * 而 force_unprepare_current_media 可能从任意线程读，边缘场景可能并发。 */
    mutable std::mutex  media_mtx_;
    GstRTSPMedia*       current_media_           = nullptr;   // 持 ref（gst_object_ref）

    /* 抑制重复 unprepare：一次错误触发后，multiple 消息可能扎堆到来，
     * 但 unprepare 只需要一次；直到 media-unprepared 回调触发后清零。 */
    std::atomic<bool>   unprepare_pending_       {false};
};

#endif //VM_IOT_RTSP_SERVER_H
