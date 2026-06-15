//
// Created by vompom on 2026/06/09.
//
// @Description
//

#include "control_channel.h"
#include "shader_filter.h"
#include "rtsp_server.h"
#include "snapshot.h"
#include "record.h"
#include "config.h"
#include "log.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <sstream>
#include <vector>

/* trim 两端空白，便于 echo 带换行/空格的健壮性。 */
static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

/* 简单按空白切词。 */
static std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(std::move(tok));
    return out;
}

bool ControlChannel::start(const std::string& req_path,
                           const std::string& reply_path,
                           ShaderFilter*      filter,
                           const Config*      cfg,
                           const RtspServer*  server,
                           Snapshot*          snapshot,
                           Record*            record,
                           std::chrono::steady_clock::time_point start_time) {
    filter_     = filter;
    cfg_        = cfg;
    server_     = server;
    snapshot_   = snapshot;
    record_     = record;
    start_time_ = start_time;

    /* 1) 请求 FIFO（必须有 filter，否则 ShaderFilter 命令无意义；但不强求 server/cfg）。 */
    if (req_path.empty()) {
        LOGI("control channel: disabled (empty req path)");
    } else {
        req_path_ = req_path;
        if (mkfifo(req_path_.c_str(), 0600) != 0 && errno != EEXIST) {
            LOGW("control channel: mkfifo({}) failed: {}", req_path_, std::strerror(errno));
            return false;
        }
        if (!reopen_fifo()) return false;
        LOGI("control channel: listening on FIFO {}", req_path_);
    }

    /* 2) 回执 FIFO（可选）。 */
    if (!reply_path.empty()) {
        reply_path_ = reply_path;
        if (mkfifo(reply_path_.c_str(), 0600) != 0 && errno != EEXIST) {
            LOGW("control channel: mkfifo reply({}) failed: {}", reply_path_, std::strerror(errno));
            reply_path_.clear();
        } else if (!open_reply_fifo()) {
            reply_path_.clear();
        } else {
            LOGI("control channel: reply FIFO ready at {}", reply_path_);
        }
    }

    return true;
}

bool ControlChannel::reopen_fifo() {
    if (watch_id_) {
        g_source_remove(watch_id_);
        watch_id_ = 0;
    }
    if (channel_) {
        g_io_channel_unref(channel_);
        channel_ = nullptr;
    }

    /* 以非阻塞 + 读写方式打开。读写方式可避免"无写端时立即 EOF"的麻烦：
     * 单纯 O_RDONLY | O_NONBLOCK 在没有写端时 read 返回 0 导致 watch 一直触发。
     * O_RDWR 让内核把我们自己也当作写端，poll/read 在无数据时阻塞等待真正写端，
     * 行为更稳定（这是 daemon 监听 FIFO 的常用 trick）。 */
    int fd = ::open(req_path_.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        LOGW("control channel: open({}) failed: {}", req_path_, std::strerror(errno));
        return false;
    }

    channel_ = g_io_channel_unix_new(fd);
    g_io_channel_set_close_on_unref(channel_, TRUE);
    g_io_channel_set_encoding(channel_, nullptr, nullptr);   // 二进制模式
    g_io_channel_set_buffered(channel_, FALSE);

    watch_id_ = g_io_add_watch(channel_,
                               (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR),
                               &ControlChannel::on_readable, this);
    return true;
}

bool ControlChannel::open_reply_fifo() {
    if (reply_fd_ >= 0) {
        ::close(reply_fd_);
        reply_fd_ = -1;
    }
    /* 同样用 O_RDWR：避免没有 reader 时 open(O_WRONLY) 阻塞或 ENXIO；
     * 我们自己持有读端，但只用写端写入。 */
    int fd = ::open(reply_path_.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        LOGW("control channel: open reply({}) failed: {}", reply_path_, std::strerror(errno));
        return false;
    }
    reply_fd_ = fd;
    return true;
}

void ControlChannel::write_reply(const std::string& payload) {
    if (reply_fd_ < 0 || payload.empty()) return;
    ssize_t n = ::write(reply_fd_, payload.data(), payload.size());
    if (n < 0) {
        /* EAGAIN：管道满（4KB 默认）或暂时无空间；EPIPE：理论上 O_RDWR 不会出现。
         * 都按"丢弃应答"处理，仅记录一次告警。 */
        LOGW("control channel: write reply failed: {} ({} bytes pending)",
             std::strerror(errno), payload.size());
    } else if (static_cast<size_t>(n) < payload.size()) {
        LOGW("control channel: short write reply ({} of {})", n, payload.size());
    }
}

void ControlChannel::stop() {
    if (watch_id_) {
        g_source_remove(watch_id_);
        watch_id_ = 0;
    }
    if (channel_) {
        g_io_channel_unref(channel_);
        channel_ = nullptr;
    }
    if (!req_path_.empty()) {
        ::unlink(req_path_.c_str());
        req_path_.clear();
    }
    if (reply_fd_ >= 0) {
        ::close(reply_fd_);
        reply_fd_ = -1;
    }
    if (!reply_path_.empty()) {
        ::unlink(reply_path_.c_str());
        reply_path_.clear();
    }
    filter_ = nullptr;
    cfg_    = nullptr;
    server_ = nullptr;
    snapshot_ = nullptr;
    record_ = nullptr;
}

gboolean ControlChannel::on_readable(GIOChannel* src, GIOCondition cond, gpointer user) {
    auto* self = static_cast<ControlChannel*>(user);
    if (!self) return G_SOURCE_REMOVE;

    if (cond & (G_IO_ERR)) {
        LOGW("control channel: G_IO_ERR, reopening");
        self->reopen_fifo();
        return G_SOURCE_REMOVE;   // 旧 watch 被 reopen 替换
    }

    /* 一次尽量读完缓冲里的所有完整行。 */
    char buf[1024];
    gsize bytes = 0;
    GError* err = nullptr;
    GIOStatus st = g_io_channel_read_chars(src, buf, sizeof(buf), &bytes, &err);
    if (err) {
        LOGW("control channel: read error: {}", err->message);
        g_error_free(err);
        return G_SOURCE_CONTINUE;
    }
    if (st == G_IO_STATUS_EOF || bytes == 0) {
        return G_SOURCE_CONTINUE;   // O_RDWR 模式下基本不会到这里
    }

    /* 简单按 \n 切行。注意：写端可能一次写半行，这里不做缓冲，
     * 假设单条命令短于 1024B 且写端用一次 write/echo 完整发出（"echo ..." 满足）。 */
    std::string chunk(buf, bytes);
    size_t pos = 0;
    while (pos < chunk.size()) {
        size_t nl = chunk.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? chunk.substr(pos)
            : chunk.substr(pos, nl - pos);
        line = trim(line);
        if (!line.empty()) self->handle_line(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return G_SOURCE_CONTINUE;
}

/* ─────────────────────────── 回执构造工具 ─────────────────────────── */
std::string ControlChannel::make_ok(const std::string& cmd_line, const std::string& body) {
    std::string out;
    out.reserve(cmd_line.size() + body.size() + 8);
    out.append("ok ").append(cmd_line).append("\n");
    if (!body.empty()) {
        out.append(body);
        if (body.back() != '\n') out.append("\n");
    }
    out.append(".\n");
    return out;
}

std::string ControlChannel::make_err(const std::string& cmd_line, const std::string& reason) {
    std::string out;
    out.reserve(cmd_line.size() + reason.size() + 8);
    out.append("err ").append(cmd_line);
    if (!reason.empty()) out.append(" ").append(reason);
    out.append("\n.\n");
    return out;
}

/* ─────────────────────────── 命令分派 ─────────────────────────── */
void ControlChannel::handle_line(const std::string& line) {
    auto toks = split_ws(line);
    if (toks.empty()) return;

    const std::string& cmd = toks[0];
    std::string reply;

    if (cmd == "filter") {
        reply = handle_filter(toks);
    } else if (cmd == "reload") {
        reply = handle_reload();
    } else if (cmd == "status") {
        reply = handle_status();
    } else if (cmd == "snapshot") {
        reply = handle_snapshot(toks);
    } else if (cmd == "record") {
        // TODO: 未来命令族扩展到1位数 (detect/motion 加入后)，
        //       考虑用 map<string, handler> 替换这里的 if-else 链。
        reply = handle_record(toks);
    } else {
        LOGW("control: unknown command '{}'", cmd);
        reply = make_err(line, "unknown_command");
    }

    write_reply(reply);
}

std::string ControlChannel::handle_filter(const std::vector<std::string>& toks) {
    const std::string& line = [&]{
        std::string s;
        for (size_t i = 0; i < toks.size(); ++i) {
            if (i) s += ' ';
            s += toks[i];
        }
        return s;
    }();

    if (!filter_) {
        LOGW("control: filter command but no ShaderFilter attached");
        return make_err(line, "filter_disabled");
    }
    if (toks.size() < 2) {
        LOGW("control: 'filter' missing argument");
        return make_err(line, "missing_argument");
    }

    const std::string& arg = toks[1];

    auto reply_with_type = [&](const std::string& action, int t, bool ok) {
        std::string body = "type=" + std::to_string(t);
        LOGI("control: filter {} -> {}", action, ok ? std::to_string(t) : "fail");
        return ok ? make_ok(line, body) : make_err(line, "apply_failed");
    };

    if (arg == "next") {
        int t = filter_->next();
        return reply_with_type("next", t, t >= 0);
    }
    if (arg == "prev") {
        int t = filter_->prev();
        return reply_with_type("prev", t, t >= 0);
    }
    if (arg == "get") {
        int t = filter_->current_type();
        LOGI("control: filter get -> {}", t);
        return make_ok(line, "type=" + std::to_string(t));
    }
    if (arg == "set") {
        if (toks.size() < 3) {
            LOGW("control: 'filter set' missing value");
            return make_err(line, "missing_value");
        }
        try {
            int t = std::stoi(toks[2]);
            bool ok = filter_->set_type(t);
            return reply_with_type("set " + toks[2], t, ok);
        } catch (...) {
            LOGW("control: invalid filter value '{}'", toks[2]);
            return make_err(line, "invalid_value");
        }
    }
    if (!arg.empty() && (std::isdigit((unsigned char)arg[0]) || arg[0] == '-')) {
        // 兼容写法：filter <N>
        try {
            int t = std::stoi(arg);
            bool ok = filter_->set_type(t);
            return reply_with_type(arg, t, ok);
        } catch (...) {
            LOGW("control: invalid filter value '{}'", arg);
            return make_err(line, "invalid_value");
        }
    }

    LOGW("control: unknown filter subcommand '{}'", arg);
    return make_err(line, "unknown_subcommand");
}

std::string ControlChannel::handle_reload() {
    if (!filter_) {
        LOGW("control: reload but no ShaderFilter attached");
        return make_err("reload", "filter_disabled");
    }
    bool ok = filter_->reload();
    LOGI("control: reload -> {}", ok ? "ok" : "fail");
    return ok ? make_ok("reload", "") : make_err("reload", "reload_failed");
}

std::string ControlChannel::handle_status() const {
    /* 状态字段尽量"自描述"：key=value 单行；外部可 grep / awk。 */
    std::ostringstream os;

    /* 1) uptime（秒，整数）。 */
    auto now = std::chrono::steady_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    os << "uptime_sec=" << secs << "\n";

    /* 2) 滤镜状态（filter 可能为 nullptr）。 */
    if (filter_) {
        os << "filter_enabled=true\n"
           << "filter_type="    << filter_->current_type() << "\n";
    } else {
        os << "filter_enabled=false\n";
    }
    if (cfg_) {
        os << "filter_max="    << cfg_->filter.max_type << "\n"
           << "filter_shader=" << cfg_->filter.shader   << "\n";
    }

    /* 3) 客户端连接数 + RTSP 入口。 */
    if (server_) {
        os << "clients=" << server_->client_count() << "\n";
    }
    if (cfg_) {
        os << "rtsp_url=rtsp://0.0.0.0:" << cfg_->server.port << cfg_->server.mount << "\n";
    }

    /* 4) 编码 & 采集快照（只读，便于运维核对配置 vs 实际）。 */
    if (cfg_) {
        os << "encoder="              << cfg_->encoder.backend       << "\n"
           << "encoder_bitrate_kbps=" << cfg_->encoder.bitrate_kbps  << "\n"
           << "capture="              << cfg_->capture.width  << "x"
                                      << cfg_->capture.height << "@"
                                      << cfg_->capture.framerate    << "\n"
           << "capture_device="       << cfg_->capture.device        << "\n"
           << "capture_pixfmt="       << cfg_->capture.pixfmt        << "\n";
    }

    /* 5) 录像副线运行态（仅在 record_ 注入时附带）。 */
    if (record_) {
        std::string rec;
        record_->format_status(rec);
        os << rec;
    }

    LOGI("control: status (uptime={}s, clients={})",
         secs, server_ ? server_->client_count() : -1);
    return make_ok("status", os.str());
}

std::string ControlChannel::handle_snapshot(const std::vector<std::string>& toks) const
{
    /* 还原命令行用于 reply 回显。 */
    std::string line;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (i) line += ' ';
        line += toks[i];
    }

    if (!snapshot_) {
        LOGW("control: snapshot but module not attached");
        return make_err(line, "snapshot_disabled");
    }
    if (!snapshot_->ready()) {
        LOGW("control: snapshot but pipeline not ready (no client connected?)");
        return make_err(line, "pipeline_not_ready");
    }

    /* 可选第二参数 = 自定义保存路径；空则由模块按时间戳生成。 */
    std::string out_path = (toks.size() >= 2) ? toks[1] : std::string();
    std::string err;
    bool ok = snapshot_->take(out_path, err);
    if (!ok) {
        LOGW("control: snapshot failed: {}", err);
        return make_err(line, err.empty() ? "unknown" : err);
    }

    LOGI("control: snapshot -> {}", out_path);
    return make_ok(line, "path=" + out_path);
}

std::string ControlChannel::handle_record(const std::vector<std::string>& toks)
{
    /* 还原命令行用于 reply 回显。 */
    std::string line;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (i) line += ' ';
        line += toks[i];
    }

    if (!record_) {
        LOGW("control: record but module not attached");
        return make_err(line, "record_disabled");
    }
    if (toks.size() < 2) {
        LOGW("control: 'record' missing subcommand");
        return make_err(line, "missing_argument");
    }

    const std::string& sub = toks[1];

    if (sub == "start") {
        std::string err;
        bool ok = record_->start(err);
        if (!ok) {
            LOGW("control: record start failed: {}", err);
            return make_err(line, err.empty() ? "unknown" : err);
        }
        LOGI("control: record start");
        return make_ok(line, "recording=true");
    }

    if (sub == "stop") {
        std::string err;
        bool ok = record_->stop_recording(err);
        if (!ok) {
            LOGW("control: record stop failed: {}", err);
            return make_err(line, err.empty() ? "unknown" : err);
        }
        LOGI("control: record stop");
        return make_ok(line, "recording=false");
    }

    if (sub == "auto") {
        if (toks.size() < 3) {
            LOGW("control: 'record auto' missing duration");
            return make_err(line, "missing_value");
        }
        int dur = 0;
        try {
            dur = std::stoi(toks[2]);
        } catch (...) {
            LOGW("control: invalid record auto duration '{}'", toks[2]);
            return make_err(line, "invalid_value");
        }
        std::string err;
        bool ok = record_->auto_record(dur, err);
        if (!ok) {
            LOGW("control: record auto failed: {}", err);
            return make_err(line, err.empty() ? "unknown" : err);
        }
        LOGI("control: record auto {}s", dur);
        return make_ok(line,
                       "recording=true\nduration_sec=" + std::to_string(dur));
    }

    if (sub == "status") {
        std::string body;
        record_->format_status(body);
        LOGI("control: record status");
        return make_ok(line, body);
    }

    LOGW("control: unknown record subcommand '{}'", sub);
    return make_err(line, "unknown_subcommand");
}
