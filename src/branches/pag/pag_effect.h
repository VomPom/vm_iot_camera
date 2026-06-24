//
// Created by vompom on 2026/06/24.
//
// @Description
//   PAG effect 子类——视频替换轨道（pageffect）形态。
//
//   特点：
//     - PAG 工程里包含"视频替换占位轨道"，运行期把底图作为素材塞进去，
//       最终输出主要是 PAG 自己的画面（底图被吃进去而不是叠加上去）；
//     - 通过 pag-replace-image-idx / pag-replace-image-every 控制替换索引
//       和节流间隔（每 N 帧上传一次纹理，避免 RGBA→PAGImage 重建过频）；
//     - 不接受 position/scale：pageffect 默认占满输出画面，sticker 的几何
//       概念在这里没有意义，由 PagSticker 提供。
//
//   ⚠️ 当前底层 pagfilter::transform_ip 对 pageffect 仍是"占坑+警告"状态，
//   本子类此时只承担"GObject 属性注入 + 热控转发"的职责；待 pagfilter 真做
//   GL 共享纹理路径后无需改本类。
//

#ifndef VM_IOT_PAG_EFFECT_H
#define VM_IOT_PAG_EFFECT_H

#include "pag_branch.h"

class PagEffect : public PagBranch {
public:
    /* effect 专属：启用/禁用画中画替换；idx>=0 启用，-1 关闭。 */
    bool set_replace_image_idx(int idx, std::string& err);
    /* effect 专属：替换上传节流间隔（每 N 帧一次，N>=1）。 */
    bool set_replace_image_every(int every, std::string& err);

protected:
    const char* branch_name() const override { return "pag_effect"; }
    /* 启动期 effect 专属属性写入。当前没有需要预置的属性
     * （replace-image-idx 默认 -1，replace-image-every 默认 2），
     * 留空实现即可；未来若引入"替换源选择"等启动期参数，写在这里。 */
    void inject_type_specific_locked(GstElement* pag0) override;
};

#endif // VM_IOT_PAG_EFFECT_H
