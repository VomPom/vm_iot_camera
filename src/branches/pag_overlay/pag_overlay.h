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
//     - 不做热切换：Stage 4.4 不支持运行时改 pag-file（pagfilter 也只允许
//       NULL/READY 改）；未来若需要热切，应在新 stage 中扩展。
//     - 不做 ControlChannel 集成：本阶段无 `pag set/get` 命令；
//     - 不接管渲染：所有像素工作都在 pagfilter 内部完成，本类只搬属性。
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
