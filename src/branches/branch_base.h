//
// Created by vompom on 2026/06/15.
//
// @Description
//   Branch 抽象基类。把所有挂在 RTSP media pipeline tee/enc_tee 后面的副线
//   （snapshot / record / detect / cloud_upload / ...）共享的"元素生命周期"
//   抽到这里，子类只填三件事：
//     1) branch_name()       —— 用于日志前缀
//     2) required_elements() —— 我要从 pipeline 抓哪些命名元素
//     3) on_attached_locked()/on_detaching_locked() —— 抓到/释放前的一次性动作
//
//   生命周期约定（子类无需自己管）：
//     attach_to_media()
//        ├── gst_rtsp_media_get_element     → pipeline_ (持 ref)
//        ├── 按 required_elements() 逐个 by_name → elements_ (各持 ref)
//        ├── 任一元素抓不到 → 全部回滚 + return（视为 attach 失败，不抛错）
//        ├── 持锁调用 on_attached_locked()  → 子类做属性注入/启动期动作
//        └── g_signal_connect("unprepared", s_on_unprepared, this)
//
//     unprepared / shutdown 都走同一条 detach_locked_() 路径：
//        ├── 持锁调用 on_detaching_locked() → 子类停 timeout/线程/probe
//        └── unref 所有 elements_ + pipeline_
//
//   关键纪律（基类强制保证，子类无从弄错）：
//     * on_detaching_locked 一定在元素 unref 之前调用 —— 子类停 timeout 时
//       元素引用还活着，可安全访问；
//     * mu_ 同时保护"元素引用"与"业务状态"，子类业务 API 与 attach/unprepared
//       天然互斥。
//
//   作用域（明确不做的事）：
//     - 不接管子类的配置加载（每种 branch 配置类型不同）；
//     - 不接管业务 API（snapshot::take / record::start 各不相同）；
//     - 不兼容"多 media 同时缓存元素"模型（ShaderFilter 那种走另一套）；
//     - 不兼容"独立进程 sink"形态（不挂在 RTSP media pipeline 内的不算 branch）。
//

#ifndef VM_IOT_BRANCH_BASE_H
#define VM_IOT_BRANCH_BASE_H

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class BranchBase {
public:
    virtual ~BranchBase();

    /* media-configure 钩子：抓元素 + 调子类 on_attached_locked + 挂 unprepared 信号。
     * 抓不到任一所需元素时记 warn 并安全返回（不影响主线 / 其它 branch）。
     * 同一个 branch 实例上重复 attach（理论上 shared=TRUE 时只会一次，但保留兼容）：
     * 旧引用会先走一次 detach 流程再绑新的。 */
    void attach_to_media(GstRTSPMedia* media);

    /* 进程退出清理：调 on_detaching_locked + 释放所有持有的 ref。
     * 多次调用幂等。 */
    void shutdown();

    /* 元素是否就绪。基类语义 = pipeline_ 已抓到（默认即认为元素也都齐）。
     * 子类若有更严的就绪条件可在自己的业务 API 里追加判断。 */
    bool ready() const;

    /* 子类填充人类可读的 status（key=value 行），默认空实现。 */
    virtual void format_status(std::string& out) const {}

protected:
    /* 用于日志前缀，例如 "snapshot" / "record"。 */
    virtual const char* branch_name() const = 0;

    /* 声明本 branch 必须从 pipeline 抓到的命名元素清单。
     * 任一元素抓不到 → 整个 attach 视为失败回滚，不部分生效。 */
    virtual std::vector<const char*> required_elements() const = 0;

    /* 元素全部抓到后调用一次（持 mu_）。子类典型用途：
     *   GstElement* sink = element("rec_sink");
     *   g_object_set(sink, "location", ..., nullptr);
     * 返回 false 表示子类自己拒绝（如配置非法），框架会把此次 attach 视作失败回滚。
     * 注意：禁止在此函数里做阻塞调用或触发 state-change。 */
    virtual bool on_attached_locked() = 0;

    /* unprepared/shutdown 的清理钩子，**先于元素 unref 调用**（持 mu_）。
     * 子类应在此停掉所有引用元素的异步活动：GLib timeout、后台线程、pad probe 等。
     * 默认空实现。 */
    virtual void on_detaching_locked() {}

    /* 通过名字取已抓到的元素（不增加 ref，仅在持锁/同步范围内使用；
     * 跨调用/跨线程使用时调用方需自己 gst_object_ref 一份）。 */
    GstElement* element(const char* name) const;

    /* 取当前 pipeline（持锁/同步范围内使用，规则同 element()）。 */
    GstElement* pipeline_locked() const { return pipeline_; }

    /* 子类与基类共用的互斥锁：同时保护元素引用与业务状态。 */
    mutable std::mutex mu_;

private:
    static void s_on_unprepared(GstRTSPMedia* media, gpointer user);

    /* attach/unprepared/shutdown 共用的释放路径。调用方必须持 mu_。 */
    void detach_locked_();

    GstElement* pipeline_ = nullptr;                          // 持 ref
    std::unordered_map<std::string, GstElement*> elements_;   // 名字→元素，各持 ref
};

#endif //VM_IOT_BRANCH_BASE_H
