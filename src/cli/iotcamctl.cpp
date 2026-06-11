// iotcamctl.cpp
// ---------------------------------------------------------------------------
// vm_iot daemon 的命令行客户端：把人话子命令翻译成 ControlChannel 协议字符串，
// 写入请求 FIFO，从回执 FIFO 读应答，并按需以人类可读 / JSON 格式输出。
//
// 设计原则：
//   1. 零业务逻辑：所有语义校验（unknown_command / invalid_value 等）都由 daemon
//      端 ControlChannel 负责，CLI 只做协议翻译 + 等待应答 + exit code 映射。
//   2. 零重型依赖：仅 libc + libstdc++，冷启动 < 100ms，不引 gst/yaml/spdlog。
//   3. 并发安全：默认对回执 FIFO 加 flock，避免两个 iotcamctl 互相串读对方应答。
//
// 协议（与 src/rtsp/control_channel.cpp 一致）：
//   请求：单行文本，\n 结尾，例如 "filter set 2\n"
//   应答：首行 "ok <cmd>" 或 "err <cmd> <reason>"，可选 body 多行，结束行单独的 "."
//
// Exit code:
//   0   daemon 回 ok
//   1   daemon 回 err（含 reason）
//   2   CLI 自身参数错误
//   10  无法打开请求/回执 FIFO（daemon 未运行 / 路径错）
//   11  抢不到回执 FIFO 锁（另一个 iotcamctl 正在跑）
//   124 等待回执超时
// ---------------------------------------------------------------------------

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* ─────────────────────────── 全局选项 ─────────────────────────── */
struct Options {
    std::string ctl_path   = "/tmp/vm_iot.ctl";
    std::string reply_path = "/tmp/vm_iot.reply";
    int  timeout_ms = 2000;
    bool json       = false;
    bool quiet      = false;
    bool no_lock    = false;
};

/* ─────────────────────────── 帮助文本 ─────────────────────────── */
void print_help() {
    std::puts(
        "iotcamctl - vm_iot daemon control client\n"
        "\n"
        "USAGE:\n"
        "  iotcamctl [GLOBAL OPTIONS] <command> [ARGS...]\n"
        "\n"
        "COMMANDS:\n"
        "  filter <N>        switch shader filter to type N (e.g. 'filter 2')\n"
        "  filter next       switch to next filter\n"
        "  filter prev       switch to previous filter\n"
        "  filter get        query current filter type\n"
        "  reload            reload shader file from disk\n"
        "  status            print runtime status (uptime, clients, encoder...)\n"
        "  snapshot [PATH]   capture one JPEG frame; PATH optional (daemon picks if empty)\n"
        "  raw \"<line>\"      send a raw protocol line (advanced)\n"
        "\n"
        "GLOBAL OPTIONS:\n"
        "  --ctl PATH        request FIFO path        [$IOTCAM_CTL or /tmp/vm_iot.ctl]\n"
        "  --reply PATH      reply FIFO path          [$IOTCAM_REPLY or /tmp/vm_iot.reply]\n"
        "  --timeout MS      reply wait timeout in ms [default: 2000]\n"
        "  --json            output JSON instead of plain text\n"
        "  -q, --quiet       suppress body output (exit code only)\n"
        "  --no-lock         skip flock on reply FIFO (use with care)\n"
        "  -h, --help        show this help\n"
        "\n"
        "EXIT CODES:\n"
        "  0   ok    1 err    2 usage    10 fifo unavailable\n"
        "  11  busy  124 timeout\n");
}

/* ─────────────────────────── 字符串工具 ─────────────────────────── */
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

/* JSON 字符串转义：仅处理必要字符，不引第三方库。 */
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

/* ─────────────────────────── 参数解析 ─────────────────────────── */
// 返回 positional 参数（命令 + 子参数），同时填 opts。
// 解析失败时调用 exit(2)。
std::vector<std::string> parse_args(int argc, char** argv, Options& opts) {
    // 先读环境变量作为默认值（命令行可覆盖）。
    if (const char* p = std::getenv("IOTCAM_CTL"))   opts.ctl_path   = p;
    if (const char* p = std::getenv("IOTCAM_REPLY")) opts.reply_path = p;

    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_val = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "iotcamctl: %s requires a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            print_help();
            std::exit(0);
        } else if (a == "--ctl") {
            opts.ctl_path = need_val("--ctl");
        } else if (a == "--reply") {
            opts.reply_path = need_val("--reply");
        } else if (a == "--timeout") {
            try {
                opts.timeout_ms = std::stoi(need_val("--timeout"));
            } catch (...) {
                std::fprintf(stderr, "iotcamctl: invalid --timeout value\n");
                std::exit(2);
            }
        } else if (a == "--json") {
            opts.json = true;
        } else if (a == "-q" || a == "--quiet") {
            opts.quiet = true;
        } else if (a == "--no-lock") {
            opts.no_lock = true;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "iotcamctl: unknown option '%s' (try --help)\n", a.c_str());
            std::exit(2);
        } else {
            pos.push_back(std::move(a));
        }
    }
    return pos;
}

/* ─────────────────────────── 子命令 → 协议行 ─────────────────────────── */
// 返回要发到请求 FIFO 的一整行（不含 \n）。失败时调 exit(2)。
std::string build_request(const std::vector<std::string>& pos) {
    if (pos.empty()) {
        std::fprintf(stderr, "iotcamctl: missing command (try --help)\n");
        std::exit(2);
    }
    const std::string& cmd = pos[0];

    if (cmd == "filter") {
        if (pos.size() < 2) {
            std::fprintf(stderr, "iotcamctl: 'filter' requires an argument (N | next | prev | get)\n");
            std::exit(2);
        }
        const std::string& arg = pos[1];
        // 数字 → set N（也兼容负号；daemon 自身会判 invalid_value）
        if (!arg.empty() && (std::isdigit((unsigned char)arg[0]) || arg[0] == '-')) {
            return "filter set " + arg;
        }
        if (arg == "next" || arg == "prev" || arg == "get") {
            return "filter " + arg;
        }
        // 透传未知子词，让 daemon 报 unknown_subcommand
        return "filter " + arg;
    }
    if (cmd == "reload" || cmd == "status") {
        if (pos.size() > 1) {
            std::fprintf(stderr, "iotcamctl: '%s' takes no argument\n", cmd.c_str());
            std::exit(2);
        }
        return cmd;
    }
    if (cmd == "snapshot") {
        // 可选传入一个路径参数；daemon 会在 reply.body 里返回 path=...
        if (pos.size() == 1) {
            return std::string("snapshot");
        }
        if (pos.size() == 2) {
            return "snapshot " + pos[1];
        }
        std::fprintf(stderr, "iotcamctl: 'snapshot' takes at most one PATH argument\n");
        std::exit(2);
    }
    if (cmd == "raw") {
        if (pos.size() < 2) {
            std::fprintf(stderr, "iotcamctl: 'raw' requires a quoted line\n");
            std::exit(2);
        }
        // 把剩余 token 用空格拼回去（shell 一般已经把引号脱掉了）
        std::string s;
        for (size_t i = 1; i < pos.size(); ++i) {
            if (i > 1) s += ' ';
            s += pos[i];
        }
        return s;
    }

    std::fprintf(stderr, "iotcamctl: unknown command '%s' (try --help)\n", cmd.c_str());
    std::exit(2);
}

/* ─────────────────────────── FIFO 读写 ─────────────────────────── */
// 检查路径是否为已存在的 FIFO；不是则报错并 exit(10)。
void check_fifo(const std::string& path, const char* role) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        std::fprintf(stderr,
                     "iotcamctl: %s FIFO '%s' not found: %s\n"
                     "  hint: is vm_iot running? check filter.control_fifo / control_reply in config\n",
                     role, path.c_str(), std::strerror(errno));
        std::exit(10);
    }
    if (!S_ISFIFO(st.st_mode)) {
        std::fprintf(stderr, "iotcamctl: %s path '%s' is not a FIFO\n", role, path.c_str());
        std::exit(10);
    }
}

// 写一行到请求 FIFO（追加 \n）。daemon 必须已经持有读端，否则 open(O_WRONLY) 会失败/阻塞。
// daemon 端使用 O_RDWR 持有读写两端，所以这里的 O_WRONLY 不会因 ENXIO 失败。
void send_request(const std::string& path, const std::string& line) {
    int fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        std::fprintf(stderr,
                     "iotcamctl: open(%s) for write failed: %s\n"
                     "  hint: is vm_iot running?\n",
                     path.c_str(), std::strerror(errno));
        std::exit(10);
    }
    std::string payload = line + "\n";
    ssize_t n = ::write(fd, payload.data(), payload.size());
    ::close(fd);
    if (n < 0 || (size_t)n != payload.size()) {
        std::fprintf(stderr, "iotcamctl: write request failed: %s\n", std::strerror(errno));
        std::exit(10);
    }
}

/* 读取应答直到看到单独一行的 "."。
 * 返回完整 payload（包含首行和 body，不含结束 "."）。
 * 超时 → exit(124)。 */
std::string read_reply(int fd, int timeout_ms) {
    std::string buf;
    buf.reserve(512);

    while (true) {
        struct pollfd pfd{ fd, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "iotcamctl: poll failed: %s\n", std::strerror(errno));
            std::exit(10);
        }
        if (pr == 0) {
            std::fprintf(stderr, "iotcamctl: timeout waiting for daemon reply (%dms)\n", timeout_ms);
            std::exit(124);
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            std::fprintf(stderr, "iotcamctl: reply FIFO error\n");
            std::exit(10);
        }

        char tmp[1024];
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            std::fprintf(stderr, "iotcamctl: read reply failed: %s\n", std::strerror(errno));
            std::exit(10);
        }
        if (n == 0) {
            // O_RDWR 持有写端，理论上不会到这；保险起见当作 EOF 重新 poll。
            continue;
        }
        buf.append(tmp, (size_t)n);

        // 检查是否已经读到结束标记：以 "\n.\n" 收尾，或 buf == ".\n"（极端空 body）。
        if (buf.size() >= 2 && buf.compare(buf.size() - 2, 2, ".\n") == 0) {
            // 必须是单独一行的 "."：前一字符要么不存在，要么是 '\n'
            if (buf.size() == 2 || buf[buf.size() - 3] == '\n') {
                // 去掉结尾的 ".\n"
                buf.erase(buf.size() - 2);
                return buf;
            }
        }
    }
}

/* ─────────────────────────── 应答解析 / 输出 ─────────────────────────── */
struct Reply {
    bool        ok = false;
    std::string cmd_line;        // 首行去掉 ok/err 前缀
    std::string error_reason;    // err 时的 reason（首行第一个 token 之后剩下的）
    std::vector<std::string> body_lines;
};

Reply parse_reply(const std::string& payload) {
    Reply r;
    // 按 \n 切；payload 不含结尾的 ".\n"
    size_t pos = 0;
    bool first = true;
    while (pos <= payload.size()) {
        size_t nl = payload.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? payload.substr(pos)
            : payload.substr(pos, nl - pos);
        if (first) {
            first = false;
            // "ok <cmd>" 或 "err <cmd> <reason>"
            if (line.rfind("ok ", 0) == 0) {
                r.ok = true;
                r.cmd_line = line.substr(3);
            } else if (line.rfind("err ", 0) == 0) {
                r.ok = false;
                std::string rest = line.substr(4);
                // err 行格式："<cmd...> <reason_token>"，reason 是最后一个 token
                size_t sp = rest.find_last_of(' ');
                if (sp == std::string::npos) {
                    r.cmd_line = rest;
                } else {
                    r.cmd_line = rest.substr(0, sp);
                    r.error_reason = rest.substr(sp + 1);
                }
            } else {
                // 协议异常：没有 ok/err 前缀。当作错误处理。
                r.ok = false;
                r.cmd_line = line;
                r.error_reason = "protocol_error";
            }
        } else {
            if (!line.empty()) r.body_lines.push_back(line);
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return r;
}

void print_plain(const Reply& r, const Options& opts) {
    if (opts.quiet) return;
    if (r.ok) {
        for (const auto& l : r.body_lines) std::puts(l.c_str());
    } else {
        std::fprintf(stderr, "error: %s",
                     r.error_reason.empty() ? "unknown" : r.error_reason.c_str());
        if (!r.cmd_line.empty()) std::fprintf(stderr, " (cmd: %s)", r.cmd_line.c_str());
        std::fputc('\n', stderr);
    }
}

void print_json(const Reply& r, const Options& opts) {
    if (opts.quiet) return;
    std::string out;
    out += "{\"ok\":";
    out += (r.ok ? "true" : "false");
    out += ",\"cmd\":\"" + json_escape(r.cmd_line) + "\"";

    if (!r.ok) {
        out += ",\"error\":\"" + json_escape(r.error_reason) + "\"";
    }

    // body：尝试按 key=value 解析为对象，失败的行塞进 raw 数组。
    out += ",\"body\":{";
    bool first_kv = true;
    std::vector<std::string> raw;
    for (const auto& l : r.body_lines) {
        size_t eq = l.find('=');
        if (eq == std::string::npos) {
            raw.push_back(l);
            continue;
        }
        std::string k = trim(l.substr(0, eq));
        std::string v = trim(l.substr(eq + 1));
        if (k.empty()) { raw.push_back(l); continue; }
        if (!first_kv) out += ',';
        first_kv = false;
        out += "\"" + json_escape(k) + "\":\"" + json_escape(v) + "\"";
    }
    out += "}";

    if (!raw.empty()) {
        out += ",\"raw\":[";
        for (size_t i = 0; i < raw.size(); ++i) {
            if (i) out += ',';
            out += "\"" + json_escape(raw[i]) + "\"";
        }
        out += "]";
    }
    out += "}\n";
    std::fputs(out.c_str(), stdout);
}

}  // namespace

/* ─────────────────────────── main ─────────────────────────── */
int main(int argc, char** argv) {
    Options opts;
    auto pos = parse_args(argc, argv, opts);

    // 1) 构造请求行（参数错误在内部 exit(2)）
    std::string line = build_request(pos);

    // 2) 检查两个 FIFO 都存在
    check_fifo(opts.ctl_path,   "control");
    check_fifo(opts.reply_path, "reply");

    // 3) 抢占式打开回执 FIFO 并加 flock，防止两个 iotcamctl 串读对方应答。
    //    注意：必须 *先* 持有 reply 端再发请求，否则 daemon 可能在我们 open 之前就把
    //    应答写完，造成竞争（虽然 daemon 用 O_RDWR 不会丢数据，但额外应答会被新 reader 读到）。
    int reply_fd = ::open(opts.reply_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (reply_fd < 0) {
        std::fprintf(stderr, "iotcamctl: open reply FIFO failed: %s\n", std::strerror(errno));
        return 10;
    }
    if (!opts.no_lock) {
        if (::flock(reply_fd, LOCK_EX | LOCK_NB) != 0) {
            std::fprintf(stderr,
                         "iotcamctl: another iotcamctl is using reply FIFO; retry later "
                         "(or use --no-lock)\n");
            ::close(reply_fd);
            return 11;
        }
    }

    // 4) 发请求
    send_request(opts.ctl_path, line);

    // 5) 读应答
    std::string payload = read_reply(reply_fd, opts.timeout_ms);
    ::close(reply_fd);  // 自动释放 flock

    // 6) 解析 + 输出
    Reply r = parse_reply(payload);
    if (opts.json) print_json(r, opts);
    else           print_plain(r, opts);

    return r.ok ? 0 : 1;
}
