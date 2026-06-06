//
// Created by vompom on on 2026/06/06 09:14.
//
// @Description
//

#ifndef VM_IOT_PIPELINE_BUILDER_H
#define VM_IOT_PIPELINE_BUILDER_H
#include <string>
#include "config.h"


class PipelineBuilder {
public:
    /* 根据配置生成 gst-rtsp-server 接受的 launch 字符串 完全一致，便于平滑迁移 */
    static std::string build(const Config &c);

private:
    /* 按 backend 返回编码器子串与所需输入像素格式 */
    static std::string encoder_str(const EncoderConfig &e, std::string &src_fmt);
};


#endif //VM_IOT_PIPELINE_BUILDER_H
