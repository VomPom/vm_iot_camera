//
// Created by vompom on 2026/06/22.
//
// @Description
//   PAG 副线（pag branch）抽象基类。
//
//   背景：
//     PipelineBuilder 在 cfg.filter.pag.enabled=true 时，会在主线 raw 段
//     插入 `pagfilter name=pag0`。pagfilter 自己的 GObject 属性 `pag-file`
//     仅在 NULL/READY 状态下可写（详见 gstpagfilter.cpp），launch 字符串里
//     直接拼路径又有转义风险（路径含空格/引号即崩 parse），因此走"启动期一次性
//     g_object_set"的方案。
//
//   双形态分裂（sticker / pageffect）：
//     1) 公共 80%——文件加载、文字层热替换、attach 生命周期、状态查询，
//        全部归到本基类 PagBranch；
//     2) 类型专属 20%——sticker 有 position/scale，pageffect 有 replace-image-*，
//        分别由子类 PagSticker / PagEffect 提供。
//
//   工作流：
//     1) main 按 cfg.filter.pag.type 选 PagSticker 或 PagEffect 实例化；
//     2) RtspServer 在 media-configure 信号里调 attach_to_media(media)；
//        BranchBase 框架按 required_elements() 抓到 pag0 元素；
//     3) on_attached_locked() 在 PAUSED 之前（pagfilter 还处于 NULL/READY）
//        先注通用属性（pag-type / pag-file / pag-text 由后续命令延迟），
//        再调虚函数 inject_type_specific_locked() 让子类注差异化属性；
//     4) 之后 pagfilter 自己在 set_caps 时按属性值 Engine::Make，渲染就生效。
//
//   作用域（明确不做的事）：
//     - 不接管渲染：所有像素工作都在 pagfilter 内部完成，本类只搬属性；
//     - 类型专属 API（set_position/set_scale/set_replace_image_*）由子类
//       提供，上层用 dynamic_cast<PagSticker*> / dynamic_cast<PagEffect*>
//       做运行期类型路由。
//
//   生命周期框架（attach/unprepared/shutdown/ready）由 BranchBase 兜底，
//   本类只填三件事：要抓哪些元素、抓到后做什么属性注入、有无需停的异步活动。
//

#ifndef VM_IOT_PAG_BRANCH_H
#define VM_IOT_PAG_BRANCH_H

#include <string>
#include <vector>

#include "branch_base.h"
#include "config.h"  /* PagFilterConfig / PagType */

/* ─────────────────────────── 抽象基类 ─────────────────────────── */
class PagBranch : public BranchBase {
public:
    /* 以运行期全量配置完成一次性注入。
     * abs_pag_file_path 是调用方解析后的绝对路径（与 selftest / shaders 一致，
     * 通常以 cfg.config_dir/.. 为基目录）。
     * cfg 传 PagFilterConfig 全部字段（type / pos_x / pos_y / scale）供基类
     * 与子类各取所需。
     * 空路径等价于"不注入"，元素维持 passthrough。 */
    virtual void configure(const PagFilterConfig& cfg, const std::string& abs_pag_file_path);

    /* ── 公共热控 API（供 ControlChannel 调用）──
     * 所有方法线程安全：内部加 BranchBase::mu_，转手调 pag0 元素的
     * GObject 属性，pagfilter 自身会把变更排队到 streaming 线程消费。
     *
     * 元素尚未 attach（pipeline 未起 / 客户端未连）时返回 false。 */

    /* 等价于在 PLAYING 状态下改 pag-file。abs_path 必须是绝对路径。 */
    bool set_pag_file(const std::string& abs_path, std::string& err);

    /* 等价于在 PLAYING 状态下改 pag-text。idx 越界由 pagfilter 内部拒绝。 */
    bool set_text(int idx, const std::string& utf8, std::string& err);

    /* 当前 PAG 文件路径 / 节流配置等只读快照，便于 status 输出。
     * 返回的字段在持锁期间从 pag0 拉，调用方不需要再加锁。 */
    struct StatusSnapshot {
        bool        attached       = false;
        std::string pag_file;
        std::string type;          // "sticker" | "pageffect"
        float       pos_x          = 0.5f;
        float       pos_y          = 0.5f;
        float       scale          = 1.0f;
        int         replace_idx    = -1;
        int         replace_every  = 2;
    };
    StatusSnapshot snapshot() const;

    /* BranchBase 钩子：默认实现，子类可叠加自身字段。 */
    void format_status(std::string& out) const override;

protected:
    /* ── BranchBase 钩子 ── */
    /* branch_name() 留给子类——使其变成抽象类，禁止直接 new PagBranch。 */
    const char* branch_name() const override = 0;
    std::vector<const char*> required_elements() const override {
        return {"pag0"};
    }
    /* media-configure 信号里同步执行（pagfilter 此时为 NULL/READY，可写）。
     * 基类负责注入 pag-type / pag-file，再调 inject_type_specific_locked()
     * 让子类填差异化属性。 */
    bool on_attached_locked() override;
    /* unprepared 时无定时器/线程需要停，留默认空实现。 */

    /* 子类专属属性注入钩子。on_attached_locked() 在通用属性注入完成后调用，
     * 此时 pag0 仍处 NULL/READY，子类可安全 g_object_set 任意属性。
     * 默认空实现；不需要差异化注入的子类可不覆盖。 */
    virtual void inject_type_specific_locked(GstElement* pag0) {}

protected:
    std::string pag_file_path_;
    PagType     type_  = PagType::Sticker;
    float       pos_x_ = 0.5f;
    float       pos_y_ = 0.5f;
    float       scale_ = 1.0f;
};

#endif //VM_IOT_PAG_BRANCH_H
