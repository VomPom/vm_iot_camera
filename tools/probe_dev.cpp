//
// tools/probe_dev.cpp
//
// 手动验证小工具：在 Linux 设备上跑 ./probe_dev /dev/video0，对比
// 系统 `v4l2-ctl --list-formats-ext` 的输出，确认我们的 V4L2Prober
// 探测结果与官方工具一致；同时用默认 RankPolicy 跑一遍 CapsRanker
// 把候选排序打印出来，验证 Stage 2 的评分结果是否符合直觉。
//
// 用法：
//   ./probe_dev                  默认探测 /dev/video0
//   ./probe_dev /dev/video2      指定设备
//

#include "caps_ranker.h"
#include "v4l2_prober.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    std::string dev = (argc >= 2) ? argv[1] : "/dev/video0";
    std::printf("Probing %s ...\n\n", dev.c_str());

    auto caps = v4l2_prober::probe(dev);
    if (caps.empty()) {
        std::printf("(空：设备打不开 / 非 Linux 平台 / 无可识别格式)\n");
        return 1;
    }

    std::printf("== 设备能力清单 ==\n%s\n",
                v4l2_prober::format_table(caps).c_str());
    std::printf("\n共 %zu 条能力记录。\n\n", caps.size());

    // 用默认策略（期望 1280x720 @ 30fps，prefer_jpeg=true）跑一次评分排序
    caps_ranker::Preference pref;
    auto ranked = caps_ranker::rank(caps, pref);

    std::printf("== 排序后的候选（期望 %ux%u@%.0ffps, prefer_jpeg=%s） ==\n",
                pref.width, pref.height, pref.fps,
                pref.prefer_jpeg ? "true" : "false");
    std::printf("%s\n", caps_ranker::format_ranking(ranked).c_str());
    return 0;
}
