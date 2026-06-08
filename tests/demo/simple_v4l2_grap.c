//
// simple_v4l2.c —— 用最少代码演示 V4L2 mmap 采集流程
// 设备：/dev/video0   格式：UYVY (YUV422 packed)   分辨率：1280x720
// 输出：每隔 50 帧把一帧 UYVY 转成 RGB 写入 export/*.ppm
//
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <errno.h>
#include <linux/videodev2.h>

#define WIDTH       1280   // 摄像头唯一支持的宽度（来自 v4l2-ctl --list-formats-ext）
#define HEIGHT      720    // 摄像头唯一支持的高度
#define BUF_COUNT   4      // mmap 缓冲区数量，循环复用
#define SAVE_EVERY  50     // 每多少帧保存一次

// 一块 mmap 缓冲区在用户空间的视图：起始地址 + 长度
struct buffer {
    void *start;
    size_t length;
};

// ioctl 失败会被 EINTR 等信号打断，这里做最小封装：失败直接退出
static void xioctl(int fd, unsigned long req, void *arg, const char *tag) {
    if (ioctl(fd, req, arg) < 0) {
        perror(tag);
        exit(EXIT_FAILURE);
    }
}

// 确保输出目录存在：不存在则创建，存在但不是目录或创建失败则退出
static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "%s 已存在但不是目录\n", path);
            exit(EXIT_FAILURE);
        }
        return;
    }
    if (errno != ENOENT || mkdir(path, 0755) < 0) {
        perror(path);
        exit(EXIT_FAILURE);
    }
}
// 把一帧 UYVY (YUV422 packed) 转成 RGB 写入 PPM
// 内存布局：每 4 字节 = U0 Y0 V0 Y1，描述 2 个相邻像素，两像素共用一对 U/V
//          整帧大小 = WIDTH * HEIGHT * 2，行 stride = WIDTH * 2
// 颜色公式：BT.601 整数版（与 yuv420_save_as_ppm 保持一致）
static void uyvy422_save_as_ppm(const uint8_t *src, const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror(filename);
        return;
    }

    fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int y = 0; y < HEIGHT; y++) {
        // 每行起点：第 y 行 × 每行字节数（WIDTH*2）
        const uint8_t *line = src + y * WIDTH * 2;
        for (int x = 0; x < WIDTH; x += 2) {
            // 一次读 4 字节，得到 2 个像素
            int u  = line[x * 2 + 0] - 128;
            int y0 = line[x * 2 + 1];
            int v  = line[x * 2 + 2] - 128;
            int y1 = line[x * 2 + 3];

            // 同一对 (u,v) 复用给 y0、y1
            int ruv =  ((1436 * v) >> 10);
            int guv = -((348  * u) >> 10) - ((731 * v) >> 10);
            int buv =  ((1812 * u) >> 10);

            int yy[2] = { y0, y1 };
            for (int k = 0; k < 2; k++) {
                int r = yy[k] + ruv;
                int g = yy[k] + guv;
                int b = yy[k] + buv;
                uint8_t rgb[3] = {
                    (uint8_t) (r < 0 ? 0 : r > 255 ? 255 : r),
                    (uint8_t) (g < 0 ? 0 : g > 255 ? 255 : g),
                    (uint8_t) (b < 0 ? 0 : b > 255 ? 255 : b),
                };
                fwrite(rgb, 1, 3, f);
            }
        }
    }
    fclose(f);
}

// 把一帧 YUV420 planar 转成 RGB 写入 PPM（BT.601 整数公式，足够直观）
static void yuv420_save_as_ppm(const uint8_t *src, const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror(filename);
        return;
    }

    // YUV420 planar 内存布局：先全部 Y，再全部 U(1/4)，最后全部 V(1/4)
    const uint8_t *Y = src;
    const uint8_t *U = Y + WIDTH * HEIGHT;
    const uint8_t *V = U + (WIDTH / 2) * (HEIGHT / 2);

    fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            // 4 个 Y 共用一对 U/V（2x2 子采样）
            int yy = Y[y * WIDTH + x];
            int uv = (y / 2) * (WIDTH / 2) + (x / 2);
            int u = U[uv] - 128;
            int v = V[uv] - 128;

            int r = yy + ((1436 * v) >> 10);
            int g = yy - ((348 * u) >> 10) - ((731 * v) >> 10);
            int b = yy + ((1812 * u) >> 10);

            uint8_t rgb[3] = {
                (uint8_t) (r < 0 ? 0 : r > 255 ? 255 : r),
                (uint8_t) (g < 0 ? 0 : g > 255 ? 255 : g),
                (uint8_t) (b < 0 ? 0 : b > 255 ? 255 : b),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

// 帧处理回调：按节流间隔写盘
static void process_frame(const void *data, int frame_idx) {
    if (frame_idx % SAVE_EVERY != 0) return;
    char path[64];
    snprintf(path, sizeof(path), "export/frame_%d.ppm", frame_idx);
    yuv420_save_as_ppm(data, path);
    printf("saved %s\n", path);
}


int main(void) {
    // ---- 0. 准备输出目录 ----
    ensure_dir("export");

    // ---- 1. 打开设备 ----
    int fd = open("/dev/video0", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/video0");
        return EXIT_FAILURE;
    }

    // ---- 2. 设置采集格式（S_FMT 后必须回读校验，否则可能被驱动静默改写）----
    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {
            .width = WIDTH,
            .height = HEIGHT,
            .pixelformat = V4L2_PIX_FMT_YUV420,
            .field = V4L2_FIELD_NONE,
        },
    };
    xioctl(fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT");
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420 ||
        fmt.fmt.pix.width != WIDTH ||
        fmt.fmt.pix.height != HEIGHT) {
        fprintf(stderr, "驱动实际生效格式与请求不符: %ux%u fmt=0x%08x\n",
                fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);
        return EXIT_FAILURE;
    }

    // ---- 3. 申请 mmap 缓冲区 ----
    struct v4l2_requestbuffers req = {
        .count = BUF_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    xioctl(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS");

    // ---- 4. 查询每个缓冲区信息、做 mmap、并入队等待内核填充 ----
    // 关键：REQBUFS 可能因为内存/对齐限制实际分配的数量 < 请求值，
    //      必须用 req.count 作为真实数量，否则 QUERYBUF 会 EINVAL
    if (req.count < 2) {
        fprintf(stderr, "分配到的缓冲区太少: %u\n", req.count);
        return EXIT_FAILURE;
    }
    struct buffer bufs[BUF_COUNT];
    for (unsigned i = 0; i < req.count; i++) {
        // v4l2_buffer 内含 union，必须整体清零，不能依赖部分字段初始化
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        xioctl(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF");
        bufs[i].length = buf.length;
        bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, buf.m.offset);
        if (bufs[i].start == MAP_FAILED) {
            perror("mmap");
            return EXIT_FAILURE;
        }
        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
    }

    // ---- 5. 启动数据流 ----
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");

    // ---- 6. 主循环：select 等待 -> DQBUF 取一帧 -> 处理 -> QBUF 还回去 ----
    for (int count = 0; ; count++) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = {.tv_sec = 2};
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
            fprintf(stderr, "select timeout / error\n");
            break;
        }

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        xioctl(fd, VIDIOC_DQBUF, &buf, "VIDIOC_DQBUF");
        process_frame(bufs[buf.index].start, count);
        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
    }

    // ---- 7. 收尾：停流 + 解除映射 + 关设备 ----
    xioctl(fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
    for (unsigned i = 0; i < req.count; i++) munmap(bufs[i].start, bufs[i].length);
    close(fd);
    return 0;
}
