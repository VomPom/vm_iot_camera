//
// Created by vompom on 2026/06/09.
//
// @Description
//   FIFO 控制通道：监听一个 named pipe（命名管道），按行解析命令并调用
//   ShaderFilter 的热切换 API，从而在不打断 RTSP 推流的前提下切换特效。
//
// ─────────────────────────── 工作原理 ───────────────────────────
//
//   1. 启动期 start()
//      ┌────────────────────────────────────────────────────┐
//      │  mkfifo(path, 0600)         // 文件系统里建一个 FIFO 节点
//      │      │  EEXIST 忽略
//      │      ▼
//      │  open(path, O_RDWR|O_NONBLOCK)
//      │      │  注：用 RDWR 而非 RDONLY，是为了让内核认为
//      │      │  我们自己也是写端，避免"无写端时 read 立即返回
//      │      │  0 (EOF) 导致 watch 空转"。这是 daemon 监听
//      │      │  FIFO 的常用 trick。
//      │      ▼
//      │  g_io_channel_unix_new(fd)  // 包成 GIOChannel
//      │      │
//      │      ▼
//      │  g_io_add_watch(IN|HUP|ERR) // 挂到默认 GMainContext
//      └────────────────────────────────────────────────────┘
//
//   2. 运行期（GMainLoop 线程内事件驱动，零轮询、零额外线程）
//
//      shell$ echo "filter next" > /tmp/vm_iot.ctl
//                       │
//                       │  内核把数据塞进 FIFO 的 pipe buffer
//                       ▼
//      GMainLoop 检测到 fd 可读 ──► on_readable(GIOChannel)
//                       │
//                       ▼
//      g_io_channel_read_chars()    // 一次读尽缓冲（≤1024B）
//                       │
//                       ▼
//      按 '\n' 切行 ──► trim ──► handle_line(line)
//                       │
//                       ▼
//      命令分派 ──► ShaderFilter::next()/prev()/set_type()/reload()
//                       │
//                       ▼
//      ShaderFilter 给所有活跃 glshader 写 uniforms 结构体
//                       │
//                       ▼
//      GStreamer 下一帧把 uniform 上传 GPU，特效切换完成
//      （不重编译 GL Program，无卡顿）
//
//   3. 异常恢复
//      G_IO_ERR ──► reopen_fifo()    // 重开 FIFO，旧 watch 移除
//      非法命令 ──► LOGW，不影响进程
//
// ─────────────────────────── 命令协议 ───────────────────────────
//   每行一条命令，UTF-8 文本，行末 '\n'：
//     filter next                  切到下一个 type（在 [0, max_type] 循环）
//     filter prev                  切到上一个 type
//     filter set <N>               直接设置 filter_type
//     filter <N>                   兼容写法，等价于 filter set <N>
//     filter get                   打印当前 type
//     reload                       重新读取 shader 文件并重新注入
//   非法命令记 warning，不会让进程崩溃。
//
// ─────────────────────────── 调试方法 ───────────────────────────
//   $ echo "filter next"   > /tmp/vm_iot.ctl
//   $ echo "filter set 2"  > /tmp/vm_iot.ctl
//   $ tail -f gst.log                              # 看 "control: ..." 日志
//

#ifndef VM_IOT_CONTROL_CHANNEL_H
#define VM_IOT_CONTROL_CHANNEL_H

#include <glib.h>
#include <string>

class ShaderFilter;

class ControlChannel {
public:
    /* path 为空串时直接返回 true 但不开启监听，便于 main 无脑调用。
     * 失败返回 false（mkfifo / open 错误）；成功后 source 已挂到默认 GMainContext。 */
    bool start(const std::string& fifo_path, ShaderFilter* filter);
    void stop();

private:
    static gboolean on_readable(GIOChannel* src, GIOCondition cond, gpointer user);
    void handle_line(const std::string& line) const;

    /* 重新打开 FIFO：写端关闭后 GIOChannel 会 EOF，必须重新 open 才能继续等下一个写端。 */
    bool reopen_fifo();

    std::string   path_;
    ShaderFilter* filter_ = nullptr;
    GIOChannel*   channel_ = nullptr;
    guint         watch_id_ = 0;
};

#endif //VM_IOT_CONTROL_CHANNEL_H
