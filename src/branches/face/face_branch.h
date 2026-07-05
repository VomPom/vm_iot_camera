//
// Created by vompom on 2026/06/30.
//
// @Description
//   人脸检测副线（face branch）。挂在 raw 锚点 t. 下游：
//
//     t. ! queue ! valve(face_valve) ! videorate ! convert ! RGB
//        ! facedetect(name=face0, display=false) ! fakesink(face_appsink)
//
//   facedetect 的检测结果不走 buffer payload，而是通过 pipeline bus 投
//   element message `Element/facedetect`，结构体里的 `faces` 字段是
//   `GValueArray of GstStructure`，每项含 `x/y/width/height` 四个 int。
//
//   FaceBranch 的职责：
//     1) attach 时抓 `face_valve` / `face0` / `face_appsink` 三件套；
//     2) 本 branch 不自己订阅 bus：RtspServer 使用
//        gst_bus_enable_sync_message_emission + "sync-message" signal，
//        在 streaming 线程同步广播给每个 branch 的 on_bus_message_sync，
//        本类在 override 里仅消费 `facedetect` 元消息与 ERROR。好处：
//          - 无 GMainContext / thread-default 陷阱 ；
//          - 不叠加 sync handler slot，不影响 rtsp-server 内部 preroll。
//     3) 解析 faces 数组（按面积降序保留前 N=8）；
//     4) 应用 cooldown_ms 节流 + emit_when_empty 策略，更新 last_state_，
//        并以 1 Hz rate-limit 打 LOGI；同时向 on_faces_cb_ 推一次 FaceEvent
//        （用于 events FIFO 推送坐标给 web 端）；
//     5) 提供 set_enabled / set_min_size / set_on_faces_callback / format_status
//        给上层调；上述 setter 全部加 mu_ 保护。
//     6) detach 时无需自己处理 bus（无订阅），直接让 BranchBase
//        unref 元素，遵守 branch_base.h 的 detach 顺序契约。
//


#ifndef VM_IOT_FACE_BRANCH_H
#define VM_IOT_FACE_BRANCH_H

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <gst/gst.h>

#include "branch_base.h"
#include "config.h"

/* 单张人脸的轴对齐矩形（来自 facedetect 的 faces[i]）。
 * 坐标系：检测帧（cfg.face 副线 RGB 帧）左上 = (0,0)。 */
struct FaceRect {
    int x = 0, y = 0, w = 0, h = 0;
    int area() const { return w * h; }
};

struct FaceEvent {
    int                                    count = 0;
    std::vector<FaceRect>                  rects;       // 按面积降序，最多 N=8
    std::chrono::steady_clock::time_point  ts{};        // 落到 last_state_ 的时刻
};

class FaceBranch : public BranchBase {
public:
    /* 启动期一次性配置；attach 之前调用，无需加锁。 */
    void configure(const FaceConfig& cfg);

    /* 控 face_valve.drop（true=不检；false=检测）。 */
    bool set_enabled(bool on, std::string* err);

    /* 热改 facedetect.min-size-width/height；自动 clamp 到 [24, 1024]。 */
    bool set_min_size(int px, std::string* err);

    /* 人脸事件回调：每当一个新事件产生（经 cooldown / emit_when_empty 过滤后），
     * FaceBranch 会向回调推一次 FaceEvent，同时携带当时检测帧的宽高（主线采集
     * 分辨率，供下游归一化坐标用）。回调会在 mu_ 临界区外同步调用，回调体内
     * 禁止反手回调 FaceBranch 公开接口以免产生重入。 */
    using OnFacesCallback =
        std::function<void(const FaceEvent& ev, int frame_w, int frame_h)>;
    void set_on_faces_callback(OnFacesCallback cb);

    /* 录入主线采集帧尺寸（cfg.capture.width/height）；回调推送时携带，供下游做归一化坐标。
     * 不设置时默认 0/0，下游可额外判断。 */
    void set_frame_size(int w, int h);

    /* status 命令用：多行 key=value 输出。 */
    void format_status(std::string& out) const override;

protected:
    const char* branch_name() const override { return "face"; }

    /* 主检测三件套必抓。 */
    std::vector<const char*> required_elements() const override {
        return {"face_valve", "face0", "face_appsink"};
    }

    bool on_attached_locked() override;
    void on_detaching_locked() override;

    /* RtspServer 在 streaming 线程广播过来的 bus 消息；内部隔离仅消费
     * `facedetect` element message 与 face0 自己的 ERROR，其余直接 return。
     * 实现里持 mu_ 的短临界区内更新 last_state_；不阻塞不重活。 */
    void on_bus_message_sync(GstMessage* msg) override;

private:
    /* facedetect element message 处理实现；由 on_bus_message_sync 转发。 */
    void            on_bus_message(GstMessage* msg);

    /* 解析 facedetect 消息里的 faces 数组到 FaceEvent；按面积降序保留前 N=8。
     * 返回 true 表示成功解析（即使 count=0 也算成功）。 */
    static bool parse_faces_message(const GstStructure* s, FaceEvent& out);

    /* 按面积降序裁到前 N=8；超过时 LOGW 一次。 */
    static constexpr int kMaxFaces = 8;

    FaceConfig                            cfg_{};
    FaceEvent                             last_state_{};
    std::chrono::steady_clock::time_point last_emit_ts_{};
    std::chrono::steady_clock::time_point last_log_ts_{};

    /* 当前主检测副线的采集帧宽高（cfg.capture.width/height），由 configure() 写入；
     * 此帧基台坐标系与 facedetect 上报的坐标系一致，用于回调中携带给下游
     * 做归一化。主线与 face 副线共享同一采集帧尺寸（videoscale后）。 */
    int                                   frame_w_ = 0;
    int                                   frame_h_ = 0;

    /* 人脸事件回调；可为空。主线写（configure/main.cpp）与 bus 线程读均在 mu_ 保护下；
     * 调用时拷一份到栈上、释放 mu_ 后再执行，避免回调内反手拿锁造成死锁。 */
    OnFacesCallback                       on_faces_cb_{};

    /* [HUNT-INSTR] bus 通路仪器：证明 sync handler 是否真的收到消息。
     * 仅调试用，稳定后应连同 on_bus_message 里的 [HUNT] 段一并移除。 */
    uint64_t                              dbg_msg_total_            = 0;
    uint64_t                              dbg_element_msg_total_    = 0;
    uint64_t                              dbg_facedetect_msg_total_ = 0;
    /* facedetect message 按 src 分开统计：face0 = 主检测；prev = preview 副线
     * 那个匿名 facedetect（gst 自动命名 facedetect0/1/…）。若两者都在稳定增长
     * 说明主线也在正常推 message，只是 count=0 被 emit_when_empty=false 静默。 */
    uint64_t                              dbg_face0_msg_total_      = 0;
    uint64_t                              dbg_face_prev_msg_total_  = 0;
    std::chrono::steady_clock::time_point dbg_last_log_{};
};

#endif // VM_IOT_FACE_BRANCH_H
