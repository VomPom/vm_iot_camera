//
// Created by vompom on 2026/06/30.
//
// @Description
//   人脸检测副线（face branch）。挂在 raw 锚点 t. 下游：
//
//     t. ! queue ! valve(face_valve) ! videorate ! convert ! RGB
//        ! facedetect(name=face0, display=false) ! appsink(face_appsink)
//
//   facedetect 的检测结果不走 buffer payload，而是通过 pipeline bus 投
//   element message `Element/facedetect`，结构体里的 `faces` 字段是
//   `GValueArray of GstStructure`，每项含 `x/y/width/height` 四个 int。
//
//   FaceBranch 的职责：
//     1) attach 时抓 `face_valve` / `face0` / `face_appsink` 三件套
//        （preview 启用时再抓 `face_prev_valve` / `face_jpeg_sink`）；
//     2) 本 branch 不自己订阅 bus：RtspServer 使用
//        gst_bus_enable_sync_message_emission + "sync-message" signal，
//        在 streaming 线程同步广播给每个 branch 的 on_bus_message_sync，
//        本类在 override 里仅消费 `facedetect` 元消息与 ERROR。好处：
//          - 无 GMainContext / thread-default 陷阱（之前处 rtsp-server client 工作线程
//            里无法拿到主 loop context 的根因完全绕开）；
//          - 不叠加 sync handler slot，不影响 rtsp-server 内部 preroll。
//     3) 解析 faces 数组（按面积降序保留前 N=8）；
//     4) 应用 cooldown_ms 节流 + emit_when_empty 策略，更新 last_state_，
//        并以 1 Hz rate-limit 打 LOGI；
//     5) 提供 set_enabled / set_min_size / set_preview / format_status
//        给 ControlChannel 调；上述 setter 全部加 mu_ 保护。
//     6) detach 时无需自己处理 bus（无订阅），直接让 BranchBase
//        unref 元素，遵守 branch_base.h 的 detach 顺序契约。
//
//   生命周期约束（沿用 BranchBase）：
//     - 单元素抓不到 → 全部回滚；
//     - on_detached_locked 先于元素 unref 调用，可安全访问 element()；
//     - mu_ 同时保护元素引用与业务状态，setter 与 attach/unprepared 互斥。
//
//   作用域（明确不做的事）：
//     - 不接管像素读取：appsink 只作"流终结点"防 pipeline 卡死；
//     - 不直接画框：画框由 preview_jpeg 副线里 display=true 的第二个
//       facedetect 实例做（与主检测路径解耦，避免画框延迟拖累主检测）；
//     - 不持久化事件：last_state_ 仅保最后一次；HTTP / SQLite 落盘留待后续。
//


#ifndef VM_IOT_FACE_BRANCH_H
#define VM_IOT_FACE_BRANCH_H

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <glib.h>
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

    /* 控 face_prev_valve.drop；preview 段未编译进 launch 时返 face_preview_disabled。 */
    bool set_preview(bool on, std::string* err);

    /* status 命令用：多行 key=value 输出。 */
    void format_status(std::string& out) const override;

    /* 当前是否有活跃的 face_prev_valve（即 preview 段已编译进 launch）。 */
    bool preview_available() const;

protected:
    const char* branch_name() const override { return "face"; }

    /* 主检测三件套必抓；preview 元素仅在编译 + yaml 都打开时才会出现在 pipeline，
     * 因此放到 attach 后用 element() 软探测，不写进 required_elements()。 */
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

    /* preview 副线元素是"可选"的：仅当 preview_jpeg.enabled=true 时才会写进
     * launch 字符串。因为可能不存在，所以不能放进 required_elements()
     *（那会触发回滚），改由 face_branch 自己在 on_attached_locked 里软抛拓：
     * gst_bin_get_by_name 拿到则持 ref，否则为 nullptr。detach 时对应 unref。 */
    GstElement*                           preview_valve_ = nullptr;   // face_prev_valve，可为 null

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
