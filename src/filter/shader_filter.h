//
// Created by vompom on 2026/06/09.
//
// @Description
//   GL 滤镜模块。负责：
//     - 把 fragment 源码注入到 pipeline 中名为 "f0" 的 glshader 元素；
//     - 跟踪所有活跃的 glshader 实例（shaders_），随 media 生命周期增删；
//     - 通过 uniform int filter_type 在运行时切换特效分支（无需重编译 GL Program）；
//     - 提供 next/prev/set/reload 给上层（ControlChannel）调用。
//
//   设计要点：
//     - 不依赖 RtspServer，也不持有完整 Config，只持有 FilterConfig 引用 + shaders_base_dir，
//       便于单测和复用；
//     - 内部 mu_ 保护 shaders_/current_type_/current_src_，回调线程安全。
//

#ifndef VM_IOT_SHADER_FILTER_H
#define VM_IOT_SHADER_FILTER_H

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <mutex>
#include <string>
#include <vector>

struct FilterConfig;

class ShaderFilter {
public:
    /* 配置后才能 attach 到 media。filter_cfg 引用必须在 ShaderFilter 生命周期内持续有效。
     * shaders_base_dir 用于把 filter_cfg.shader 中的相对路径解析为绝对路径。 */
    void configure(const FilterConfig& filter_cfg, const std::string& shaders_base_dir);

    /* 由 RtspServer 在 media-configure 时回调：
     *   - 找到 pipeline 中 name=f0 的 glshader；
     *   - 注入 fragment 源码（首帧前必须完成，否则 caps 协商时无 program）；
     *   - 写入初始 uniforms（filter_type）；
     *   - 监听 media 的 unprepared 信号自动从集合中移除。 */
    void attach_to_media(GstRTSPMedia* media);

    /* 析构期清理：unref 所有 shader 引用，复位状态。 */
    void shutdown();

    /* ---------------- 热切换 API ---------------- */
    int  next();                // 返回新的 type；失败 -1
    int  prev();                // 返回新的 type；失败 -1
    bool set_type(int type);    // 越界 false
    bool reload();              // 重读 fragment 文件并写回所有活跃 shader
    int  current_type() const;

    /* 是否启用滤镜（filter_cfg.enabled 的快照）。
     * RtspServer / main 用它决定是否要 attach。 */
    bool enabled() const;

private:
    /* 调用方必须持有 mu_。失败时不更新 current_type_。 */
    bool apply_type_locked(int type);

    /* 给单个 glshader 写一次 uniforms（GstStructure 形式）。 */
    void push_uniforms(GstElement* shader, int type) const;

    /* 解析 filter_cfg_.shader 为绝对路径（基目录 = shaders_base_dir_）。 */
    std::string resolve_shader_path() const;

    /* media unprepared 信号回调：从 shaders_ 中移除对应 glshader。 */
    static void on_media_unprepared(GstRTSPMedia* media, gpointer user);

    const FilterConfig*  filter_cfg_ = nullptr;
    std::string          shaders_base_dir_;

    mutable std::mutex   mu_;
    std::vector<GstElement*> shaders_;   // 已 ref，shutdown 时 unref
    int                  current_type_ = -1;
    std::string          current_src_;   // 最近一次成功加载的 fragment 源码
};

#endif //VM_IOT_SHADER_FILTER_H
