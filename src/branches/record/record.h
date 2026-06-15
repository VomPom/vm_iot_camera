//
// Created by vompom on 2026/06/15.
//
// @Description
//   录像副线（record branch）控制模块。
//
//   GStreamer 端结构（由 PipelineBuilder 拼装）：
//       enc_t. ! queue name=rec_queue (no-leaky, 2s 缓冲)
//             ! valve name=rec_valve drop=true
//             ! splitmuxsink name=rec_sink muxer-factory=mp4mux
//                            max-size-time=<segment_sec*1e9>
//                            send-keyframe-requests=true
//                            async-finalize=true
//                            location=<会被本模块改写>
//
//   工作流（关键：splitmuxsink 不感知 valve 状态，stop 必须显式触发切片）：
//     1) on_attached_locked()：挂 "format-location" 信号（生成最终文件名）+ 在
//        pipeline bus 上挂 sync-handler 监听 "splitmuxsink-fragment-closed"
//        element message（用作"上一段 mp4 已落盘 / 写完 moov"的同步事件）。
//        按 cfg.enabled 决定是否开 valve。
//     2) start()：valve 开闸（splitmuxsink 看到数据流后会自动开新 fragment，
//        fragment_id 自增；format-location 回调拿到新 id 生成新文件名）。
//     3) stop_recording()：
//          ① emit "split-now"（让 splitmuxsink 当前段封口，async 写 moov）
//          ② 等 "splitmuxsink-fragment-closed" message（最多 1s）
//          ③ valve 关闸（防止后续数据再喂进 splitmuxsink）
//        多次 start/stop 会产生多个独立 mp4 文件（fragment_id 0/1/2/...）。
//     4) on_detaching_locked()（unprepared / shutdown）：
//          - recording_=false：直接清理（上一次 stop 已经 finalize 过）；
//          - recording_=true ：valve 关闸 → 给 rec_queue.src 发 EOS →
//                              等 fragment-closed 或 1s 超时 → 清理。
//
//   线程模型：
//     - 控制 API（start/stop/auto/status）在 GMainLoop 线程同步调用；
//     - bus sync-handler 在元素流线程触发，**不持业务锁**，仅唤醒 fragment_cv_；
//     - 锁：复用 BranchBase::mu_ 保护元素引用与状态机；fragment_cv_ 与 mu_ 配套。
//
//   注：磁盘容量管理（保留 N 段 / 总大小上限）暂未实现，由用户/运维自行通过 logrotate
//       或外部清理脚本管理 cfg.dir。
//

#ifndef VM_IOT_RECORD_H
#define VM_IOT_RECORD_H

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <condition_variable>
#include <string>
#include <vector>

#include "branch_base.h"
#include "config.h"

class Record : public BranchBase {
public:
    /* 配置参数（启动期一次性）。dir 不存在会尝试 mkdir -p。 */
    void configure(const RecordConfig& cfg);

    /* ── 控制 API（GMainLoop 线程同步） ── */

    /* 开闸（手动模式）。已经在录则视作幂等成功。 */
    bool start(std::string& err);

    /* 关闸：split-now 让当前段 finalize，等待写盘（≤1s 超时）后关闸。
     * 已经停了视作幂等成功。 */
    bool stop_recording(std::string& err);

    /* 一次性定时录制：开闸 + duration_sec 后自动 stop_recording。
     * duration_sec<=0 视作非法。已经在录则替换定时器（先停旧的 timeout 再装新的）。 */
    bool auto_record(int duration_sec, std::string& err);

    /* 是否正在录（valve 开 + 元素就绪）。 */
    bool is_recording() const;

    /* 用于 status 命令：填充人类可读的 key=value 行。 */
    void format_status(std::string& out) const override;

protected:
    /* ── BranchBase 钩子 ── */
    const char* branch_name() const override { return "record"; }
    std::vector<const char*> required_elements() const override {
        return {"rec_queue", "rec_valve", "rec_sink"};
    }
    /* 元素抓到后：写 location、挂 format-location 信号、挂 bus sync-handler，
     * 按 cfg.enabled 决定是否开闸。 */
    bool on_attached_locked() override;
    /* unprepared/shutdown 前：finalize 当前段（必要时发 EOS）+ 摘 sync-handler。 */
    void on_detaching_locked() override;

private:
    /* GLib timeout：auto 模式到点回调，触发停止。 */
    static gboolean on_auto_timeout(gpointer user);

    /* 在锁内拨 valve.drop。 */
    void set_valve_drop_locked(bool drop);

    /* 检查 rec_valve / rec_sink 是否就绪；不就绪时把 err 设为 "pipeline_not_ready"。
     * 调用方持 mu_。 */
    bool ensure_ready_locked_(std::string& err) const;

    /* 撤销可能存在的 auto 定时器（手动 start/stop 与 detach 都要做）。
     * 调用方持 mu_。 */
    void cancel_auto_timeout_locked_();

    /* splitmuxsink "format-location" 信号回调（在流线程触发）。
     * 返回值由 splitmuxsink g_free，必须用 g_strdup / g_strdup_printf 之类分配。 */
    static gchar* on_format_location(GstElement* sink, guint fragment_id, gpointer user);

    /* pipeline bus "sync-message::element" 信号回调：仅识别
     * "splitmuxsink-fragment-closed" element message，唤醒 fragment_cv_。
     * 使用 sync-message 信号订阅（而非 gst_bus_set_sync_handler），以避免
     * 覆盖 GstRTSPMedia 在 prepare 阶段自己挂的 sync_handler。
     * **不得持业务锁、不得阻塞**——本回调跑在元素流线程上。 */
    static void on_sync_message(GstBus* bus, GstMessage* msg, gpointer user);

    /* 在持有 mu_ 的前提下等 fragment-closed 通知（已 emit split-now 或 EOS 之后调）。
     * 超时返回 false（只 warn 不阻塞调用方）。timeout_ms<=0 表示不等。 */
    bool wait_fragment_closed_locked_(int timeout_ms);

    /* 给 rec_queue.src pad 注入 EOS（仅 shutdown 时录制中调用）。 */
    void inject_eos_locked_();

    RecordConfig cfg_{};
    bool         recording_           = false;   // valve 是否已开
    guint        auto_timeout_id_     = 0;       // 0=无定时器
    gulong       format_loc_handler_  = 0;       // "format-location" handler id；0=未挂
    bool         sync_handler_set_    = false;   // 是否已在 pipeline bus 启用 sync-message
    GstBus*      bus_ref_             = nullptr; // 持 ref；detach 时用以摘 signal + disable emission
    gulong       sync_msg_handler_    = 0;       // "sync-message::element" handler id

    /* 段封口同步：bus sync-handler 把 fragment_closed_pending_ 置 true 并 notify_all；
     * 控制 API 在 emit split-now 之前先把它置 false，emit 之后等 cv 通知。 */
    std::condition_variable fragment_cv_;
    bool                    fragment_closed_pending_ = false;
};

#endif //VM_IOT_RECORD_H
