// Copyright (c) 2025 vm_iot
//
// EventFifoWriter：daemon 侧的单向事件广播 FIFO 写端。
//
// 与 ControlChannel 的定位差异：
//   - ControlChannel 是"请求-应答"通道（web/CLI 发命令、daemon 回执行结果），
//     持双 FIFO：request_fifo / reply_fifo。
//   - EventFifoWriter 是"事件广播"通道，daemon 单向写、web 单向读，
//     用一根 FIFO（event_fifo）承载 NDJSON（每行一个 JSON 对象 + '\n'）。
//     读端可以随时热接热断；无读端时 daemon 侧丢弃并按秒级节流打 warn。
//
// 设计要点：
//   1) O_RDWR|O_NONBLOCK 打开自己的 FIFO：常见 trick，避免"没读端时 open
//      RDWR 会挂起"或"没读端时 write EPIPE"，与 ControlChannel::reply_fd_
//      的做法一致。
//   2) 独立 writer 线程 + 有界队列：GStreamer bus 线程调 push_faces() 时
//      **绝不能被 write 阻塞**——即使非阻塞写在管道满时也要立即返回；
//      我们再加一层内部队列，让 push_faces() 100% 无 IO、O(1) 入队即返回。
//      队列满时（默认 32 条）丢最旧，节流后打 warn。
//   3) 序列化极简：手写 NDJSON。不引入 json 库依赖，与本项目其他 FIFO 消息
//      同风格。schema 见 push_faces() 注释。
//   4) 幂等 start/stop：多次调用安全；stop() 必须在析构或 daemon 关闭前
//      显式调用，以确保 writer 线程 join。
//
// 线程模型：
//   - 写者：任意线程调 push_faces()（当前是 GStreamer streaming 线程）。
//   - 内部：独立 writer 线程从队列取 + 非阻塞 write 到 fd。
//   - 读者：外部 web 进程按行读 event_fifo。

#ifndef VM_IOT_EVENT_FIFO_WRITER_H
#define VM_IOT_EVENT_FIFO_WRITER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "face_branch.h"

class EventFifoWriter {
public:
    EventFifoWriter() = default;
    ~EventFifoWriter();

    EventFifoWriter(const EventFifoWriter&)            = delete;
    EventFifoWriter& operator=(const EventFifoWriter&) = delete;

    /* 打开事件 FIFO：不存在则 mkfifo，随后 O_RDWR|O_NONBLOCK 打开并起 writer 线程。
     * path 为空串或 mkfifo/open 失败 → 静默禁用（返回 false）；调用方按需打日志。
     * 幂等：已 start 时直接返回 true。 */
    bool start(const std::string& path);

    /* 关闭：置退出标志、唤醒 writer 线程、join，然后 close(fd)。幂等。 */
    void stop();

    /* 是否已成功打开可写。 */
    bool active() const { return running_.load(std::memory_order_acquire); }

    /* 推送一次 face 事件（NDJSON 格式，schema）：
     *   {"kind":"faces","ts_ns":<int64>,"frame_w":<int>,"frame_h":<int>,
     *    "count":<int>,
     *    "rects":[{"x":<int>,"y":<int>,"w":<int>,"h":<int>}, ...]}
     * 语义：
     *   - ts_ns 为 daemon 侧 steady_clock 单调时间戳（web 端仅做相对推算，
     *     不与本地 wall clock 比较）；
     *   - frame_w/h 为主线采集帧尺寸（cfg.capture.width/height），rects 内
     *     的坐标即在此尺寸下测得，前端做归一化时以此为基。
     *   - rects 已由 FaceBranch 侧按面积降序裁到前 N（默认 8）。
     * 线程安全；O(1)；队列满时丢最旧并按秒级节流 warn。 */
    void push_faces(const FaceEvent& ev, int frame_w, int frame_h);

private:
    void writer_loop();

    /* 把 FaceEvent 序列化到一行 NDJSON，末尾附 '\n'。 */
    static std::string serialize_faces(const FaceEvent& ev,
                                       int frame_w, int frame_h);

    /* 同步向 fd 尝试非阻塞 write；EAGAIN 视为“无读端 / 管道满”，返回 false。
     * 其他 I/O 错误也返回 false，由调用方节流打 warn。 */
    bool try_write_line(const std::string& line);

    static constexpr size_t kQueueMax = 32;

    std::string                     path_;
    int                             fd_       = -1;
    std::atomic<bool>               running_{false};
    std::atomic<bool>               stop_flag_{false};

    std::thread                     worker_;
    std::mutex                      mu_;
    std::condition_variable         cv_;
    std::deque<std::string>         queue_;         // 待写行（含末尾 '\n'）
    uint64_t                        dropped_total_ = 0;
    std::chrono::steady_clock::time_point last_drop_warn_ts_{};
};

#endif  // VM_IOT_EVENT_FIFO_WRITER_H
