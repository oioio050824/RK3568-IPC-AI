/**
 * v4l2_capture.cpp — V4L2 摄像头采集实现
 *
 * 从 Phase 1 main.cpp 提取, 封装为独立线程函数。
 * 关键设计: 采集线程永不阻塞 — memcpy 在锁外, 锁内仅 swap+signal。
 *
 * Phase 7.3 改动: 每帧同时产出两份 NV12 副本 ——
 *   一份给编码 (主码流), 一份给 AI 检测 (旁路, 独立单槽/锁)。
 *   两个消费者互不抢锁, 采集线程也永不因 AI 慢而阻塞。
 */

#include "v4l2_capture.h"

#include <iostream>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

// ---- 辅助: 可中断 ioctl ----
static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

// ---- 辅助: 微秒时间戳 ----
static int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

void* capture_thread(void* arg) {
    SharedState* s = static_cast<SharedState*>(arg);
    int W = s->width, H = s->height;
    size_t fsize = (size_t)W * H * 3 / 2;

    // ---- 1. 打开设备 ----
    int fd = open("/dev/video0", O_RDWR);
    if (fd < 0) {
        std::cerr << "[采集] ❌ 无法打开 /dev/video0: " << std::strerror(errno) << std::endl;
        return nullptr;
    }

    // ---- 2. 设置格式 (NV12, Multiplanar) ----
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = (uint32_t)W;
    fmt.fmt.pix_mp.height      = (uint32_t)H;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "[采集] ❌ VIDIOC_S_FMT 失败: " << std::strerror(errno) << std::endl;
        close(fd); return nullptr;
    }
    std::cout << "[采集] " << W << "x" << H << " NV12, " << fsize << "B/帧" << std::endl;

    // ---- 3. 申请 MMAP 缓冲区 ----
    v4l2_requestbuffers req{};
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count  = 4;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "[采集] ❌ VIDIOC_REQBUFS 失败: " << std::strerror(errno) << std::endl;
        close(fd); return nullptr;
    }

    struct MmapBuf { void* ptr; size_t len; };
    std::vector<MmapBuf> bufs(req.count);
    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_buffer b{};
        v4l2_plane  planes[8]{};
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = i;
        b.m.planes = planes;
        b.length   = 8;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            std::cerr << "[采集] ❌ VIDIOC_QUERYBUF " << i << " 失败" << std::endl;
            close(fd); return nullptr;
        }
        bufs[i].len = b.m.planes[0].length;
        bufs[i].ptr = mmap(nullptr, b.m.planes[0].length,
                           PROT_READ | PROT_WRITE, MAP_SHARED,
                           fd, b.m.planes[0].m.mem_offset);
        if (bufs[i].ptr == MAP_FAILED) {
            std::cerr << "[采集] ❌ mmap " << i << " 失败" << std::endl;
            close(fd); return nullptr;
        }
    }

    // ---- 4. 入队所有缓冲区 + 开启采集 ----
    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_buffer b{};
        v4l2_plane  planes[8]{};
        b.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory   = V4L2_MEMORY_MMAP;
        b.index    = i;
        b.m.planes = planes;
        b.length   = 8;
        xioctl(fd, VIDIOC_QBUF, &b);
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "[采集] ❌ VIDIOC_STREAMON 失败" << std::endl;
        for (auto& x : bufs) if (x.ptr && x.ptr != MAP_FAILED) munmap(x.ptr, x.len);
        close(fd); return nullptr;
    }

    // ---- 5. 丢 9 帧等待 3A 收敛 ----
    for (int i = 0; i < 9; i++) {
        v4l2_buffer b{};
        v4l2_plane  planes[8]{};
        b.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory   = V4L2_MEMORY_MMAP;
        b.m.planes = planes;
        b.length   = 8;
        if (xioctl(fd, VIDIOC_DQBUF, &b) == 0)
            xioctl(fd, VIDIOC_QBUF, &b);
    }
    std::cout << "[采集] 3A 收敛完成, 开始采集" << std::endl;

    // ---- 6. 主采集循环 ----
    int64_t t0       = now_us();
    int64_t last_log = t0;
    int frame_cnt    = 0, last_cnt = 0;

    while (s->running) {
        // poll 可中断等待 (1s 超时用于检查 running 标志)
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;  // 超时, 回到 while 检查 running

        // 取出帧
        v4l2_buffer b{};
        v4l2_plane  planes[8]{};
        b.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory   = V4L2_MEMORY_MMAP;
        b.m.planes = planes;
        b.length   = 8;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // ═══ 关键: memcpy 在锁外 ═══
        int64_t pts = now_us() - t0;
        size_t bytes = b.m.planes[0].bytesused;

        Nv12Frame local;                       // 给编码 (主码流)
        local.copy_from((uint8_t*)bufs[b.index].ptr, bytes, pts);

        Nv12Frame local_ai;                    // 给 AI (旁路, 独立副本)
        local_ai.copy_from((uint8_t*)bufs[b.index].ptr, bytes, pts);

        // 归还缓冲区给 ISP (必须在锁外 — 这是一个 ioctl)
        xioctl(fd, VIDIOC_QBUF, &b);

        // ═══ 锁内只做 swap + signal (耗时 < 1μs) ═══
        pthread_mutex_lock(&s->nv12_mutex);
        s->nv12.data.swap(local.data);  // O(1) 指针交换, 无内存拷贝
        s->nv12.pts_us = local.pts_us;
        s->nv12_seq++;
        pthread_cond_signal(&s->nv12_cond);
        pthread_mutex_unlock(&s->nv12_mutex);

        // AI 槽独立锁, 与编码槽互不阻塞
        pthread_mutex_lock(&s->ai_nv12_mutex);
        s->ai_nv12.data.swap(local_ai.data);
        s->ai_nv12.pts_us = local_ai.pts_us;
        s->ai_nv12_seq++;
        pthread_cond_signal(&s->ai_nv12_cond);
        pthread_mutex_unlock(&s->ai_nv12_mutex);

        // FPS 日志
        frame_cnt++;
        if ((frame_cnt % 30) == 0) {
            int64_t now = now_us();
            double fps = (double)(frame_cnt - last_cnt) * 1e6 / (double)(now - last_log);
            std::cout << "[采集] " << frame_cnt << " 帧 ("
                      << (int)(fps + 0.5) << "fps)" << std::endl;
            last_log = now;
            last_cnt = frame_cnt;
        }
    }

    // ---- 7. 清理 ----
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (auto& x : bufs)
        if (x.ptr && x.ptr != MAP_FAILED)
            munmap(x.ptr, x.len);
    close(fd);

    // 唤醒编码线程和 AI 线程使其退出
    pthread_cond_signal(&s->nv12_cond);
    pthread_cond_signal(&s->ai_nv12_cond);

    std::cout << "[采集] 退出 (" << frame_cnt << " 帧)" << std::endl;
    return nullptr;
}
