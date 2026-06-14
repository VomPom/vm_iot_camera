//
// Created by vompom on 2026/06/11.
//
// @Description
//   gst-launch 字符串可读化打印工具（header-only）。
//
//   背景：
//     PipelineBuilder::build() 输出的 launch 字符串被压成一整行，
//     加入 tee 分流后元素一多就根本看不清拓扑。本工具把这串字符串
//     按 GStreamer launch 语法做最小解析（不引第三方库），按
//     "采集主链 / 各 tee 副线" 分组，每个元素一行，带缩进箭头。
//
//   适用范围：
//     - 仅服务调试日志（LOGI），不参与 pipeline 构造，不要拿来"反序列化"。
//     - 只识别 PipelineBuilder 实际产出的形态：
//         (
//           src ! caps ! convert ... ! tee name=t
//           t. ! queue ! ... ! payX
//           t. ! queue ! ... ! sinkY
//         )
//       其它复杂场景（嵌套 bin、多 tee、命名分支引用 a.|b.）会退化为
//       "尽力而为"，至少保持每个 ! 一行的可读性。
//
//   设计要点：
//     - 纯 std::string 实现，不依赖正则，避免 libstdc++ <regex> 体积。
//     - 不修改输入字符串语义，只做拆分和重排版。
//     - 输出是单一多行 std::string，调用方自行决定 LOGI 一次性吐还是分行吐。
//

#ifndef VM_IOT_LAUNCH_PRETTY_H
#define VM_IOT_LAUNCH_PRETTY_H

#include <string>
#include <vector>
#include <sstream>
#include <cctype>

namespace launch_pretty {

/* ---------- 内部工具：字符串处理 ---------- */

inline std::string _trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

/* 把外层一对括号去掉（PipelineBuilder 总是用 "( ... )" 包裹）。
 * 不是闭合或不存在就原样返回。 */
inline std::string _strip_outer_parens(const std::string& s) {
    std::string t = _trim(s);
    if (t.size() >= 2 && t.front() == '(' && t.back() == ')') {
        return _trim(t.substr(1, t.size() - 2));
    }
    return t;
}

/* 按顶层 "!" 拆分（GStreamer launch 用 ! 串联元素）。
 * 我们的 launch 字符串里 ! 只出现在元素之间，不会在 quoted 字符串里，
 * 因此简单按 ' ! ' 切就够了。 */
inline std::vector<std::string> _split_by_bang(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        // 严格匹配 " ! " 或行首/行尾紧贴的 "!"，避免误伤元素属性中潜在的 ! （目前没有）。
        bool is_bang =
            (s[i] == '!') &&
            (i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1]))) &&
            (i + 1 >= s.size() || std::isspace(static_cast<unsigned char>(s[i + 1])));
        if (is_bang) {
            out.push_back(_trim(cur));
            cur.clear();
        } else {
            cur += s[i];
        }
    }
    if (!_trim(cur).empty()) out.push_back(_trim(cur));
    return out;
}

/* 判断一个 token 是不是 "<name>." 形式的 tee 引用起点（如 "t."）。
 * 这种 token 在 launch 里相当于"从已命名 bin 上拉一条新线"。 */
inline bool _is_branch_anchor(const std::string& tok, std::string& tee_name) {
    if (tok.size() < 2) return false;
    if (tok.back() != '.') return false;
    for (size_t i = 0; i + 1 < tok.size(); ++i) {
        char c = tok[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    tee_name = tok.substr(0, tok.size() - 1);
    return true;
}

/* 判断一个字符串片段是否形如 "<name>."（tee 锚点形式），
 * <name> 仅包含字母数字和下划线，且必须以字母开头（避免误吞数字结尾元素）。 */
inline bool _looks_like_anchor(const std::string& s) {
    if (s.size() < 2) return false;
    if (s.back() != '.') return false;
    if (!std::isalpha(static_cast<unsigned char>(s.front()))) return false;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        char c = s[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

/* 把首尾粘连的 "<name>." 锚点从 token 里拆出来，作为独立 token。
 *
 * 背景：
 *   PipelineBuilder 产出的串里 tee 分支起点形如：
 *       ... ! tee name=t t. ! queue ... ! pay0 t. ! queue ... ! sink
 *   按 ' ! ' 切完后，会得到：
 *       ["v4l2src ...", "... ! tee name=t t.",
 *        "queue ... ! ... ! pay0 t.",
 *        "queue ... ! ... ! sink"]
 *   末尾的 " t." 粘在了上一个 token 尾巴上，导致后续无法识别成分支锚点。
 *
 * 处理：
 *   遍历每个 token，
 *     - Step A: 如果以 "<name>. " 开头（后面是空格），把锚点剥到独立 token；
 *     - Step B: 如果以 " <name>." 结尾（前面是空格），把锚点剥到独立 token；
 *   两步都用循环，能容忍极端的多次粘连，正常输入下各跑一次就收敛。
 */
inline std::vector<std::string> _detach_anchors(const std::vector<std::string>& in) {
    std::vector<std::string> out;
    out.reserve(in.size());
    for (const auto& raw : in) {
        std::string t = _trim(raw);

        // Step A: 剥头部粘连的锚点
        while (true) {
            auto sp = t.find_first_of(" \t");
            if (sp == std::string::npos) break;
            std::string head = _trim(t.substr(0, sp));
            if (!_looks_like_anchor(head)) break;
            out.push_back(head);
            t = _trim(t.substr(sp + 1));
            if (t.empty()) break;
        }
        if (t.empty()) continue;

        // Step B: 剥尾部粘连的锚点。注意尾部锚点逻辑上属于"下一段"的起点，
        // 所以要先 push 当前 token 主体，再倒序 push 这些尾部锚点。
        std::vector<std::string> trailing;
        while (true) {
            auto sp = t.find_last_of(" \t");
            if (sp == std::string::npos) break;
            std::string tail = _trim(t.substr(sp + 1));
            if (!_looks_like_anchor(tail)) break;
            trailing.push_back(tail);
            t = _trim(t.substr(0, sp));
            if (t.empty()) break;
        }

        if (!t.empty()) out.push_back(t);
        // trailing 是从右往左剥下来的，原序应当还原成"最右边的最后追加"。
        for (auto it = trailing.rbegin(); it != trailing.rend(); ++it) {
            out.push_back(*it);
        }
    }
    return out;
}

/* 从一个元素 token 里抓 name=xxx，返回空表示没有。
 * 仅用于副线启发式命名（snap_valve -> snapshot, rec_valve -> record ...）。 */
inline std::string _extract_name(const std::string& tok) {
    const std::string key = "name=";
    auto p = tok.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    std::string v;
    while (p < tok.size() && !std::isspace(static_cast<unsigned char>(tok[p]))) {
        v += tok[p++];
    }
    return v;
}

/* 给元素 token 取一个简短"元素类型"（第一个空格前的内容，去掉 caps 串的 video/x-raw 形式）。
 * 仅用于给副线起标题。 */
inline std::string _element_kind(const std::string& tok) {
    auto sp = tok.find(' ');
    return (sp == std::string::npos) ? tok : tok.substr(0, sp);
}

/* ---------- 副线标题推断 ----------
 * 启发式：扫一遍副线 tokens，如果发现 name=<prefix>_xxx，就用 prefix 当分支名。
 * 找不到就用 "branch_N" 兜底。 */
inline std::string _guess_branch_title(const std::vector<std::string>& toks,
                                       int branch_idx) {
    for (const auto& t : toks) {
        std::string n = _extract_name(t);
        auto us = n.find('_');
        if (us != std::string::npos && us > 0) {
            std::string prefix = n.substr(0, us);
            // 已知 prefix 美化映射；未知 prefix 直接用 prefix 本身
            if (prefix == "snap") return "snapshot";
            if (prefix == "rec")  return "record";
            if (prefix == "ai")   return "ai";
            if (prefix == "pay")  return "main";   // pay0 是 RTP 主线 sink
            return prefix;
        }
        // 没有 _ 但 name=pay0/pay1 也按主线处理
        if (n.rfind("pay", 0) == 0) return "main";
    }
    return "branch_" + std::to_string(branch_idx);
}

/* ---------- 单段渲染 ---------- */

/* 把一段连续的 tokens（从 src 或 tee. 起，到下一个段为止）渲染成
 *   line1
 *     └─► line2
 *         └─► line3
 *             ...
 * 不限制层级深度（用空格累加）。 */
inline std::string _render_chain(const std::vector<std::string>& toks,
                                 int base_indent) {
    std::ostringstream os;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (i == 0) {
            os << std::string(base_indent, ' ') << toks[i] << '\n';
        } else {
            int ind = base_indent + static_cast<int>(i) * 4;
            os << std::string(ind, ' ') << "└─► " << toks[i] << '\n';
        }
    }
    return os.str();
}

/* ---------- 主入口 ----------
 * 输入：PipelineBuilder::build() 产出的 launch 字符串（含外层括号）。
 * 输出：多行人类可读的拓扑。
 *
 * 解析步骤：
 *   1) 去外层括号；
 *   2) 顶层按 ! 切成 token 流；
 *   3) 遇到 "<name>." 形式 token 视为分支边界，前面累计的归当前段，
 *      该 token 自身只用于打印 "from <name>." 提示，不进入 tokens。
 *   4) 第一段是 source 段（v4l2src 起头）；其余段都是副线。
 */
inline std::string render(const std::string& launch) {
    std::ostringstream os;
    os << "================ pipeline (gst-launch) ================\n";

    std::string body = _strip_outer_parens(launch);
    std::vector<std::string> toks = _split_by_bang(body);
    // 关键：把 "tee name=t t." 这种尾部粘连 / 头部粘连的分支锚点拆成独立 token，
    // 否则下面按 anchor 分段会错把整条流当成一根直链。
    toks = _detach_anchors(toks);

    // 以 "<name>." 作为段边界，把 tokens 分组
    struct Segment {
        std::string  anchor;           // 空 = source 段；否则 "t" 之类 tee 名
        std::vector<std::string> toks; // 段内的元素链（不含 anchor 自身）
    };
    std::vector<Segment> segs;
    Segment cur;
    cur.anchor.clear();
    for (auto& t : toks) {
        std::string anchor;
        if (_is_branch_anchor(t, anchor)) {
            if (!cur.toks.empty() || !cur.anchor.empty()) {
                segs.push_back(std::move(cur));
                cur = Segment{};
            }
            cur.anchor = anchor;
        } else {
            cur.toks.push_back(t);
        }
    }
    if (!cur.toks.empty() || !cur.anchor.empty()) {
        segs.push_back(std::move(cur));
    }

    // 打印
    int branch_idx = 0;
    for (size_t i = 0; i < segs.size(); ++i) {
        const auto& s = segs[i];
        if (s.anchor.empty()) {
            os << "[source]\n";
            os << _render_chain(s.toks, /*base_indent*/ 2);
        } else {
            std::string title = _guess_branch_title(s.toks, branch_idx++);
            os << "\n[branch:" << title << "]"
               << "   ◄── from " << s.anchor << ".\n";
            os << _render_chain(s.toks, /*base_indent*/ 2);
        }
    }

    os << "========================================================";
    return os.str();
}

} // namespace launch_pretty

#endif // VM_IOT_LAUNCH_PRETTY_H
