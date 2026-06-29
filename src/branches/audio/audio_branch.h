//
// Created by vm_iot on 2026/06/29.
//
// @Description
//   音频副线（audio branch）。模式与 PagBranch 对齐：BranchBase 框架
//   负责"抓元素 / detach 释放"，本类只填三件事：
//     1) required_elements()：抓 aud_vol + aud_valve；
//     2) on_attached_locked()：把启动期 volume / mute 一次性写到元素；
//     3) 业务热控 API：set_volume / set_mute；
//
//   生命周期与 ControlChannel 配合：
//     - cfg.audio.enabled=false 时 main 不构造本对象，pipeline 也不出现 aud_*；
//     - cfg.audio.enabled=true 但客户端没连上时 attached=false，set_* 返
//       "not_attached"，由 ControlChannel 转译给 cli。
//
//   作用域（明确不做的事）：
//     - 不接管 RTP / 编码：那部分由 PipelineBuilder 在 launch 字符串里完成；
//     - 不接管 RTSP 媒体生命周期：BranchBase 已绑 unprepared 信号兜底；
//     - 不接管 mute 与 volume 的相互独立性：valve.drop 与 volume.volume 是
//       两个正交属性，volume=0 + drop=false 同样能"听感静音"，但仍有 RTP 包；
//       mute=true + volume=1 才是真正不发包。
//

#ifndef VM_IOT_AUDIO_BRANCH_H
#define VM_IOT_AUDIO_BRANCH_H

#include <string>
#include <vector>

#include "branch_base.h"
#include "config.h"

class AudioBranch : public BranchBase {
public:
    /* 启动期单次配置；attach 之前调用，无需加锁。 */
    void configure(const AudioConfig& cfg);

    /* ── 热控 API（供 ControlChannel 调用）──
     * 元素未 attach 时返 false，err="not_attached"。 */
    bool set_volume(float v, std::string& err);
    bool set_mute  (bool  m, std::string& err);

    /* status 输出供 ControlChannel handle_status 拼接。 */
    void format_status(std::string& out) const override;

protected:
    const char* branch_name() const override { return "audio"; }
    std::vector<const char*> required_elements() const override {
        return {"aud_vol", "aud_valve"};
    }
    bool on_attached_locked() override;
    /* 无 timeout / 后台线程，detach 留默认空实现。 */

private:
    /* 启动期配置快照；attach 时从这里推到 GObject 属性。
     * 之后 set_* 只更新元素属性，不回填这里——cfg 是"启动配置事实"，
     * 运行期实时值通过 g_object_get 取得（format_status 即如此）。 */
    AudioConfig cfg_{};
};

#endif // VM_IOT_AUDIO_BRANCH_H
