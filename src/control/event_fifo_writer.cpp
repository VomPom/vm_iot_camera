// Copyright (c) 2025 vm_iot
//
// EventFifoWriter 实现细节说明见头文件。这里只做几件事：
//   1) 打开 FIFO：mkfifo (幂等) + O_RDWR|O_NONBLOCK。
//   2) 起独立 writer 线程：wait 队列，取行做非阻塞 write。
//   3) push_faces()：序列化到 NDJSON，压队列，通知 writer；O(1)。
//
// 这个类**不感知**读端来自 web 还是 shell（`cat /tmp/vm_iot.events`），
// 无读端时 writes 拿到 EAGAIN 就把该行丢掉，保证 daemon 前台不因外部消费者
// 掉线而阻塞或崩溃。

#include "event_fifo_writer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

#include "log.h"

EventFifoWriter::~EventFifoWriter() {
    stop();
}

bool EventFifoWriter::start(const std::string& path) {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (path.empty()) {
        return false;
    }

    /* mkfifo 若已存在返回 EEXIST，不算错。其他错误直接放弃。 */
    if (::mkfifo(path.c_str(), 0666) != 0 && errno != EEXIST) {
        LOGW("event_fifo: mkfifo('{}') failed: {}", path, std::strerror(errno));
        return false;
    }

    /* O_RDWR + O_NONBLOCK：
     *   - RDWR 保证在没有读端时 open 也不阻塞、后续 write 不会 EPIPE；
     *   - NONBLOCK 让 write 在管道满 / 无读端持续读取时立即返回 EAGAIN，
     *     由 writer 线程决定是重试还是丢弃。 */
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        LOGW("event_fifo: open('{}', O_RDWR|O_NONBLOCK) failed: {}",
             path, std::strerror(errno));
        return false;
    }

    fd_        = fd;
    path_      = path;
    stop_flag_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    worker_    = std::thread(&EventFifoWriter::writer_loop, this);

    LOGI("event_fifo: started, path='{}', queue_max={}", path_, kQueueMax);
    return true;
}

void EventFifoWriter::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    stop_flag_.store(true, std::memory_order_release);
    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    running_.store(false, std::memory_order_release);
    LOGI("event_fifo: stopped, dropped_total={}", dropped_total_);
}

void EventFifoWriter::push_faces(const FaceEvent& ev,
                                 int frame_w, int frame_h) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    std::string line = serialize_faces(ev, frame_w, frame_h);

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (queue_.size() >= kQueueMax) {
            /* 队列满：丢最旧一条，保证最新数据能进队；节流打 warn 避免刷屏。 */
            queue_.pop_front();
            ++dropped_total_;
            auto now = std::chrono::steady_clock::now();
            if (now - last_drop_warn_ts_ >= std::chrono::seconds(1)) {
                last_drop_warn_ts_ = now;
                LOGW("event_fifo: queue full, dropped oldest (total={})",
                     dropped_total_);
            }
        }
        queue_.emplace_back(std::move(line));
    }
    cv_.notify_one();
}

std::string EventFifoWriter::serialize_faces(const FaceEvent& ev,
                                             int frame_w, int frame_h) {
    /* 手写 NDJSON 拼接：坐标全为整数，不需要 escape 字符串字段。
     * 单行 JSON 对象 + 末尾 '\n'。schema 见头文件。 */
    std::string out;
    out.reserve(64 + ev.rects.size() * 48);

    /* ts_ns：steady_clock 单调时钟；web 端只做相对推算，不与 wall clock 比较。 */
    const auto ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           ev.ts.time_since_epoch())
                           .count();

    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "{\"kind\":\"faces\",\"ts_ns\":%lld,\"frame_w\":%d,\"frame_h\":%d,\"count\":%d,\"rects\":[",
                  static_cast<long long>(ts_ns), frame_w, frame_h, ev.count);
    out.append(buf);

    for (size_t i = 0; i < ev.rects.size(); ++i) {
        const auto& r = ev.rects[i];
        std::snprintf(buf, sizeof(buf),
                      "%s{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                      i == 0 ? "" : ",", r.x, r.y, r.w, r.h);
        out.append(buf);
    }
    out.append("]}\n");
    return out;
}

bool EventFifoWriter::try_write_line(const std::string& line) {
    if (fd_ < 0) return false;

    const char*  p    = line.data();
    size_t       left = line.size();
    while (left > 0) {
        ssize_t n = ::write(fd_, p, left);
        if (n > 0) {
            p    += n;
            left -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* 无读端 / 管道满：本次消息丢弃（NDJSON 允许缺行，读端自行容错），
             * 由 push_faces 侧的秒级节流打 warn。 */
            return false;
        }
        /* 其他 I/O 错误（EIO / EBADF 等）：返回失败，writer_loop 会节流打日志。 */
        LOGW("event_fifo: write failed: {}", std::strerror(errno));
        return false;
    }
    return true;
}

void EventFifoWriter::writer_loop() {
    while (true) {
        std::string line;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] {
                return stop_flag_.load(std::memory_order_acquire) ||
                       !queue_.empty();
            });
            if (stop_flag_.load(std::memory_order_acquire) && queue_.empty()) {
                return;
            }
            line = std::move(queue_.front());
            queue_.pop_front();
        }
        (void)try_write_line(line);
    }
}
