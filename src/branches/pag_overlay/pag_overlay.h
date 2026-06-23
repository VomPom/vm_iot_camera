//
// Created by vompom on 2026/06/22.
//
// @Description
//   PAG 叠加副线（pag overlay branch）启动期注入模块。
//
//   背景：
//     PipelineBuilder 在 cfg.filter.pag.enabled=true 时，会在主线 raw 段
//     插入 `pagfilter name=pag0`。pagfilter 自己的 GObject 属性 `pag-file`
//     仅在 NULL/READY 状态下可写（详见 gstpagfilter.cpp），launch 字符串里
//     直接拼路径又有转义风险（路径含空格/引号即崩 parse），因此走"启动期一次性
//     g_object_set"的方案。
//
//   工作流：
//     1) RtspServer 在 media-configure 信号里调 attach_to_media(media)；
//        BranchBase 框架按 required_elements() 抓到 pag0 元素；
//     2) on_attached_locked() 在 PAUSED 之前（pagfilter 还处于 NULL/READY）
//        执行 `g_object_set(pag0, "pag-file", abs_path, nullptr)`；
//     3) 之后 pagfilter 自己在 set_caps 时按属性值 Engine::Make，渲染就生效了。
//
//   作用域（明确不做的事）：
//     - 不接管渲染：所有像素工作都在 pagfilter 内部完成，本类只搬属性。
//     - 运行时热控：提供 set_pag_file / set_pag_text / set_pag_replace_image*
//       接口，上层（ControlChannel）调用后转手写 pag0 元素属性。
//
//   生命周期框架（attach/unprepared/shutdown/ready）由 BranchBase 兜底，
//   本类只填三件事：要抓哪些元素、抓到后做什么属性注入、有无需停的异步活动。
//

#ifndef VM_IOT_PAG_OVERLAY_H
#define VM_IOT_PAG_OVERLAY_H

#include <string>
#include <vector>

#include "branch_base.h"

class PagOverlay : public BranchBase {
public:
    /* 配置 PAG 资源绝对路径。
     * 调用方负责把相对路径解析成绝对路径（与 selftest / shaders 一致，
     * 通常以 cfg.config_dir/.. 为基目录），本类只忠实搬运。
     * 空字符串等价于"不注入"，元素维持 passthrough。 */
    void configure(const std::string& abs_pag_file_path);

    /* ── 运行时热控 API（供 ControlChannel 调用）──
     * 所有方法线程安全：内部加 BranchBase::mu_，转手调 pag0 元素的
     * GObject 属性，pagfilter 自身会把变更排队到 streaming 线程消费。
     *
     * 元素尚未 attach（pipeline 未起 / 客户端未连）时返回 false。 */

    /* 等价于在 PLAYING 状态下改 pag-file。abs_path 必须是绝对路径。 */
    bool set_pag_file(const std::string& abs_path, std::string& err);

    /* 等价于在 PLAYING 状态下改 pag-text。idx 越界由 pagfilter 内部拒绝。 */
    bool set_text(int idx, const std::string& utf8, std::string& err);

    /* 配置 pag-replace-image-idx：>=0 启用画中画替换，-1 关闭。 */
    bool set_replace_image_idx(int idx, std::string& err);

    /* 配置 pag-replace-image-every：节流间隔（>=1）。 */
    bool set_replace_image_every(int every, std::string& err);

    /* 当前 PAG 文件路径 / 节流配置等只读快照，便于 status 输出。
     * 返回的字段在持锁期间从 pag0 拉，调用方不需要再加锁。 */
    struct StatusSnapshot {
        bool        attached       = false;
        std::string pag_file;
        int         replace_idx    = -1;
        int         replace_every  = 2;
    };
    StatusSnapshot snapshot() const;

    /* BranchBase 钩子：未实现，默认空。 */
    void format_status(std::string& out) const override;

protected:
    /* ── BranchBase 钩子 ── */
    const char* branch_name() const override { return "pag_overlay"; }
    std::vector<const char*> required_elements() const override {
        return {"pag0"};
    }
    /* media-configure 信号里同步执行（pagfilter 此时为 NULL/READY，可写）。 */
    bool on_attached_locked() override;
    /* unprepared 时无定时器/线程需要停，留默认空实现。 */

private:
    std::string pag_file_path_;
};

#endif //VM_IOT_PAG_OVERLAY_H
