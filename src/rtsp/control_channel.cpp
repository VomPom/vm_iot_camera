//
// Created by vompom on 2026/06/09.
//
// @Description
//

#include "control_channel.h"
#include "shader_filter.h"
#include "log.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
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

bool ControlChannel::start(const std::string& fifo_path, ShaderFilter* filter) {
    if (fifo_path.empty()) {
        LOGI("control channel: disabled (empty path)");
        return true;
    }
    path_ = fifo_path;
    filter_ = filter;

    /* mkfifo 已存在则忽略 EEXIST。其它错误（比如目录不存在）直接告警并放弃。 */
    if (mkfifo(path_.c_str(), 0600) != 0 && errno != EEXIST) {
        LOGW("control channel: mkfifo({}) failed: {}", path_, std::strerror(errno));
        return false;
    }

    if (!reopen_fifo()) return false;

    LOGI("control channel: listening on FIFO {}", path_);
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
    int fd = ::open(path_.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        LOGW("control channel: open({}) failed: {}", path_, std::strerror(errno));
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

void ControlChannel::stop() {
    if (watch_id_) {
        g_source_remove(watch_id_);
        watch_id_ = 0;
    }
    if (channel_) {
        g_io_channel_unref(channel_);
        channel_ = nullptr;
    }
    if (!path_.empty()) {
        ::unlink(path_.c_str());   // 清理 FIFO 节点
        path_.clear();
    }
    filter_ = nullptr;
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

void ControlChannel::handle_line(const std::string& line) const {
    if (!filter_) return;
    auto toks = split_ws(line);
    if (toks.empty()) return;

    const std::string& cmd = toks[0];

    if (cmd == "filter") {
        if (toks.size() < 2) {
            LOGW("control: 'filter' missing argument");
            return;
        }
        const std::string& arg = toks[1];
        if (arg == "next") {
            int t = filter_->next();
            LOGI("control: filter next -> {}", t);
        } else if (arg == "prev") {
            int t = filter_->prev();
            LOGI("control: filter prev -> {}", t);
        } else if (arg == "get") {
            LOGI("control: filter get -> {}", filter_->current_type());
        } else if (arg == "set") {
            if (toks.size() < 3) {
                LOGW("control: 'filter set' missing value");
                return;
            }
            try {
                int t = std::stoi(toks[2]);
                bool ok = filter_->set_type(t);
                LOGI("control: filter set {} -> {}", t, ok ? "ok" : "fail");
            } catch (...) {
                LOGW("control: invalid filter value '{}'", toks[2]);
            }
        } else if (!arg.empty() && (std::isdigit((unsigned char)arg[0]) || arg[0] == '-')) {
            // 兼容写法：filter <N>
            try {
                int t = std::stoi(arg);
                bool ok = filter_->set_type(t);
                LOGI("control: filter {} -> {}", t, ok ? "ok" : "fail");
            } catch (...) {
                LOGW("control: invalid filter value '{}'", arg);
            }
        } else {
            LOGW("control: unknown filter subcommand '{}'", arg);
        }
    } else if (cmd == "reload") {
        bool ok = filter_->reload();
        LOGI("control: reload -> {}", ok ? "ok" : "fail");
    } else {
        LOGW("control: unknown command '{}'", cmd);
    }
}
