//
// Created by vompom on 2026/06/24.
//
// @Description
//   PAG sticker 子类——视频叠加（overlay）形态。
//
//   特点：
//     - PAG 工程里只用普通图层（图像/文字/形状），渲染结果是 RGBA 位图，
//       由 pagfilter 在 transform_ip 里 alpha-blend 到底图上；
//     - 支持 position/scale 控制：position 是 PAG 画面中心对齐底图归一化坐标，
//       scale 是 PAG 自身缩放系数（详见 gstpagfilter.cpp 内 RGBA 缩放/定位逻辑）；
//     - 不消费"视频替换轨道"：set_replace_image_* 不属于本子类，由 PagEffect 提供。
//

#ifndef VM_IOT_PAG_STICKER_H
#define VM_IOT_PAG_STICKER_H

#include "pag_branch.h"

class PagSticker : public PagBranch {
public:
    /* sticker 专属：运行期调位置（归一化 [0,1]，中心对齐）。 */
    bool set_position(float x, float y, std::string& err);
    /* sticker 专属：运行期调缩放（>0）。 */
    bool set_scale(float scale, std::string& err);

protected:
    const char* branch_name() const override { return "pag_sticker"; }
    /* 在基类注入 pag-type/file 之间，把 pos+scale 三个属性写好；
     * pagfilter 后续 set_caps 加载素材时即可立刻用到。 */
    void inject_type_specific_locked(GstElement* pag0) override;
};

#endif // VM_IOT_PAG_STICKER_H
