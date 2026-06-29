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
//     status                       打印运行状态（uptime / 客户端数 / 编码 / 滤镜 / 录像等）
//     snapshot [PATH]              抓一张 jpeg 落盘
//     audio status                 打印音频副线运行状态
//     audio mute on/off            主线 valve.drop 静音开关
//     audio volume <v>             主线 volume.volume 调音量，[0,10]
//   非法命令记 warning，不会让进程崩溃。
//
// ─────────────────────────── 回执协议 ───────────────────────────
//   配置 control.reply_fifo 后，daemon 会把每条命令的应答写入该 FIFO：
//     成功：  ok <cmd...>\n[k=v\n]*.\n        // 终结符为单独一行的 "."
//     失败：  err <cmd...> <reason>\n.\n
//   外部用法：
//     $ cat /tmp/vm_iot.reply &        # 后台读
//     $ echo "status" > /tmp/vm_iot.ctl
//   注意：reply 是非阻塞写，若读端不存在会丢弃应答（仅日志告警）。
//
// ─────────────────────────── 调试方法 ───────────────────────────
//   $ echo "filter next"   > /tmp/vm_iot.ctl
//   $ echo "filter set 2"  > /tmp/vm_iot.ctl
//   $ tail -f gst.log                              # 看 "control: ..." 日志
//

#ifndef VM_IOT_CONTROL_CHANNEL_H
#define VM_IOT_CONTROL_CHANNEL_H

#include <glib.h>
#include <chrono>
#include <string>
#include <vector>

class ShaderFilter;
class RtspServer;
class Snapshot;
class PagBranch;
class AudioBranch;
struct Config;

class ControlChannel {
public:
    /* 启动控制通道。
     *   req_path / reply_path 为空串时分别表示"不开请求 FIFO"/"不写回执"。
     *   filter / cfg / server / snapshot 必须在 ControlChannel 生命周期内保持存活。
     *   pag_branch 可为 nullptr（filter.pag.enabled=false 时）；非空时启用
     *   `pag set-*` 命令族。运行期会根据实际类型 dynamic_cast 到
     *   PagSticker / PagEffect 路由差异化子命令。
     *   start_time 用于 status 命令计算 uptime；通常传 main 启动时记录的 steady_clock::now()。
     * 失败返回 false（mkfifo / open 错误）；成功后 source 已挂到默认 GMainContext。 */
    bool start(const std::string& req_path,
               const std::string& reply_path,
               ShaderFilter*      filter,
               const Config*      cfg,
               const RtspServer*  server,
               Snapshot*          snapshot,
               PagBranch*         pag_branch,
               AudioBranch*       audio_branch,
               // TODO(record): 重开录像时在此恢复 Record* record 参数
               std::chrono::steady_clock::time_point start_time);
    void stop();

private:
    static gboolean on_readable(GIOChannel* src, GIOCondition cond, gpointer user);
    void handle_line(const std::string& line);

    /* 重新打开请求 FIFO：写端关闭后 GIOChannel 会 EOF，必须重新 open 才能继续等下一个写端。 */
    bool reopen_fifo();

    /* 打开 / 重开 reply FIFO（O_RDWR | O_NONBLOCK，避免无读端时 open 阻塞）。 */
    bool open_reply_fifo();

    /* 把一段已构造好的应答字符串非阻塞写入 reply FIFO。
     * 没配置 reply / 写失败时仅 LOGW，不影响主流程。 */
    void write_reply(const std::string& payload);

    /* 各命令实现：返回 reply 字符串（含终结符 ".\n"）。 */
    std::string handle_filter(const std::vector<std::string>& toks);
    std::string handle_reload();
    std::string handle_status() const;
    std::string handle_snapshot(const std::vector<std::string>& toks) const;
    std::string handle_record(const std::vector<std::string>& toks);
    /* PAG 命令族（set-file / set-text / set-replace-image / get）。
     * 与上面命令一样：原始命令行已 trim/split，本函数负责再 join 出回执的 cmd 部分。
     * pag_branch_ 为 nullptr 时所有子命令统一返 "pag_disabled"。
     * sticker 专属子命令（未来 set-position/set-scale）需 dynamic_cast<PagSticker*>；
     * effect 专属子命令 set-replace-image* 需 dynamic_cast<PagEffect*>。 */
    std::string handle_pag(const std::vector<std::string>& toks);

    /* 音频命令族：
     *   audio status                  输出 AudioBranch::format_status
     *   audio mute on/off             aud_valve.drop
     *   audio volume <v>              aud_vol.volume
     * audio_branch_ 为 nullptr 时返 "audio_disabled"。 */
    std::string handle_audio(const std::vector<std::string>& toks);

    /* 工具：构造 "ok <line>\n<body>.\n" / "err <line> <reason>\n.\n"。 */
    static std::string make_ok(const std::string& cmd_line, const std::string& body);
    static std::string make_err(const std::string& cmd_line, const std::string& reason);

    std::string   req_path_;
    std::string   reply_path_;
    ShaderFilter* filter_  = nullptr;
    const Config* cfg_     = nullptr;
    const RtspServer* server_ = nullptr;
    Snapshot*     snapshot_ = nullptr;
    PagBranch*    pag_branch_ = nullptr;
    AudioBranch*  audio_branch_ = nullptr;
    std::chrono::steady_clock::time_point start_time_{};

    GIOChannel*   channel_ = nullptr;
    guint         watch_id_ = 0;
    int           reply_fd_ = -1;        // -1 = 未启用或未打开
};

#endif //VM_IOT_CONTROL_CHANNEL_H
