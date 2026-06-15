//
// Created by vompom on 2026/06/11.
//
// @Description
//   截图副线（snapshot branch）控制模块。
//
//   GStreamer 端结构（由 PipelineBuilder 拼装）：
//       tee. ! queue ! valve name=snap_valve drop=true
//                    ! videoconvert ! jpegenc ! multifilesink name=snap_sink
//
//   工作流（take 一次截图）：
//     1) 改写 snap_sink 的 location 属性 → 目标路径；
//     2) 在 snap_sink 的 sink pad 上挂一次性 BUFFER probe；
//     3) 把 snap_valve 的 drop 设为 false（开闸）；
//     4) probe 看到一个 buffer 通过 sink（multifilesink 是同步 write，
//        到达 sink pad 即视为已落盘），通过 condition_variable 唤醒主线程；
//     5) 主线程立刻关闸（drop=true）并移除 probe；
//     6) 超时未收到 → 关闸 + 返回错误。
//
//   线程模型：
//     - take() 在 ControlChannel 的 GMainLoop 线程同步阻塞调用；
//     - probe 回调在 GStreamer 流线程触发，用互斥+条件变量唤醒；
//     - 不去碰 gst-rtsp-server 内部 pipeline 的 bus，避免
//       'timeout == 0 || bus->priv->poll != NULL' 断言。
//
//   生命周期框架（attach/unprepared/shutdown/ready）由 BranchBase 兜底，
//   本类只填三件事：要抓哪些元素、抓到后做什么、卸载前停什么。
//


#ifndef VM_IOT_SNAPSHOT_H
#define VM_IOT_SNAPSHOT_H

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "branch_base.h"

class Snapshot : public BranchBase {
public:
    /* 配置默认输出目录、JPEG 质量、单次截图等待超时（毫秒）。
     * dir 为空时，take() 必须显式传入完整路径。 */
    void configure(const std::string& dir,
                   int                quality_unused = 90,
                   int                timeout_ms     = 1500);

    /* 同步触发一次截图。
     * @param out_path  in/out：传空则由模块按时间戳生成路径写到 dir/，
     *                  传非空则按该路径落盘；返回时被改写为最终保存路径。
     * @param err       失败原因（pipeline_not_ready / write_failed / timeout / invalid_path）。
     * 返回 true=成功；false=err 已填。 */
    bool take(std::string& out_path, std::string& err) const;

protected:
    /* ── BranchBase 钩子 ── */
    const char* branch_name() const override { return "snapshot"; }
    std::vector<const char*> required_elements() const override {
        return {"snap_valve", "snap_sink"};
    }
    bool on_attached_locked() override { return true; }   // 无一次性属性需要写
    /* unprepared 时无定时器/线程需要停，留默认空实现。 */

private:
    /* sink pad buffer probe：buffer 到达即视为该帧已写盘，唤醒等待者。 */
    static GstPadProbeReturn on_sink_buffer(GstPad* pad, GstPadProbeInfo* info, gpointer user);

    /* 生成 "<dir>/snap_YYYYMMDD_HHMMSS_mmm.jpg" 风格路径。 */
    std::string make_default_path() const;

    std::string dir_;
    int         timeout_ms_ = 1500;

    /* 单次截图同步原语（take() 内独占使用，take_mu_ 保证串行）。 */
    mutable std::mutex              take_mu_;
    mutable std::mutex              cv_mu_;
    mutable std::condition_variable cv_;
    mutable bool                    got_buffer_ = false;
};

#endif //VM_IOT_SNAPSHOT_H
