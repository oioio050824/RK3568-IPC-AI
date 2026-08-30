/**
 * Phase 3 辅助工具: V4L2 连续采集 N 帧 NV12
 *
 * Phase 1 只抓一帧。RTSP 推流需要多帧，所以这个程序:
 *   STREAMON → 丢9帧(3A收敛) → 循环取N帧 → STREAMOFF
 *   所有帧连续写入一个文件 (raw NV12, 可直接喂 Phase 2 编码器)
 *
 * 编译: aarch64-rockchip-linux-gnu-g++ -std=c++17 -O2 -o capture_multi main.cpp
 * 用法: ./capture_multi [/dev/video0] [output.yuv] [width] [height] [num_frames]
 *
 * 输出: 单文件, 大小 = W*H*3/2 * num_frames
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "[错误] " << msg << ": " << std::strerror(errno)      \
                      << std::endl;                                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do { r = ioctl(fd, request, arg); } while (r == -1 && errno == EINTR);
    return r;
}

int main(int argc, char *argv[]) {
    const char *dev_name  = (argc > 1) ? argv[1] : "/dev/video0";
    const char *out_file  = (argc > 2) ? argv[2] : "capture_multi.yuv";
    int req_width         = (argc > 3) ? std::stoi(argv[3]) : 640;
    int req_height        = (argc > 4) ? std::stoi(argv[4]) : 480;
    int num_frames        = (argc > 5) ? std::stoi(argv[5]) : 30;

    std::cout << "=== Phase 3 辅助: 连续采集 " << num_frames << " 帧 ===" << std::endl;
    std::cout << "设备: " << dev_name << ", 分辨率: " << req_width << "x" << req_height << std::endl;

    // ---- 1. 打开设备 -------------------------------------------------------
    int fd = ::open(dev_name, O_RDWR);
    CHECK(fd >= 0, "无法打开设备");

    // ---- 2. 查询设备能力 ---------------------------------------------------
    v4l2_capability cap{};
    CHECK(xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0, "VIDIOC_QUERYCAP");
    std::cout << "驱动: " << cap.driver << std::endl;
    CHECK(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE, "不是 Multiplanar 采集设备");
    CHECK(cap.capabilities & V4L2_CAP_STREAMING, "不支持流式采集");

    // ---- 3. 设置 NV12 格式 ------------------------------------------------
    v4l2_format fmt{};
    fmt.type                   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = static_cast<__u32>(req_width);
    fmt.fmt.pix_mp.height      = static_cast<__u32>(req_height);
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    CHECK(xioctl(fd, VIDIOC_S_FMT, &fmt) == 0, "VIDIOC_S_FMT");

    int width  = fmt.fmt.pix_mp.width;
    int height = fmt.fmt.pix_mp.height;
    size_t frame_size = width * height * 3 / 2;
    std::cout << "实际: " << width << "x" << height
              << ", 单帧 " << frame_size << " 字节" << std::endl;

    // ---- 4. 申请 MMAP 缓冲区 ----------------------------------------------
    v4l2_requestbuffers req{};
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count  = 4;
    CHECK(xioctl(fd, VIDIOC_REQBUFS, &req) == 0, "VIDIOC_REQBUFS");
    CHECK(req.count >= 2, "缓冲区不足");

    struct Buffer {
        void      *start  = nullptr;
        size_t     length = 0;
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
    };
    std::vector<Buffer> buffers(req.count);

    for (unsigned i = 0; i < req.count; i++) {
        v4l2_buffer buf{};
        v4l2_plane  planes[VIDEO_MAX_PLANES]{};
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.m.planes = planes;
        buf.length   = VIDEO_MAX_PLANES;
        CHECK(xioctl(fd, VIDIOC_QUERYBUF, &buf) == 0, "VIDIOC_QUERYBUF");
        buffers[i].length    = buf.m.planes[0].length;
        buffers[i].planes[0] = buf.m.planes[0];
        buffers[i].start = mmap(nullptr, buf.m.planes[0].length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                fd, buf.m.planes[0].m.mem_offset);
        CHECK(buffers[i].start != MAP_FAILED, "mmap");
    }

    // ---- 5. 入队 + STREAMON ------------------------------------------------
    for (unsigned i = 0; i < req.count; i++) {
        v4l2_buffer buf{};
        v4l2_plane  planes[VIDEO_MAX_PLANES]{};
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.m.planes = planes;
        buf.length   = VIDEO_MAX_PLANES;
        CHECK(xioctl(fd, VIDIOC_QBUF, &buf) == 0, "VIDIOC_QBUF");
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    CHECK(xioctl(fd, VIDIOC_STREAMON, &type) == 0, "VIDIOC_STREAMON");
    std::cout << "采集已启动, 丢掉前9帧 (3A收敛)..." << std::endl;

    // ---- 6. 丢9帧 ---------------------------------------------------------
    for (int i = 0; i < 9; i++) {
        v4l2_buffer drop{};
        v4l2_plane  dp[VIDEO_MAX_PLANES]{};
        drop.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        drop.memory   = V4L2_MEMORY_MMAP;
        drop.m.planes = dp;
        drop.length   = VIDEO_MAX_PLANES;
        if (xioctl(fd, VIDIOC_DQBUF, &drop) == 0)
            xioctl(fd, VIDIOC_QBUF, &drop);
    }
    std::cout << "[3A收敛完成]" << std::endl;

    // ---- 7. 采集 N 帧 -----------------------------------------------------
    std::ofstream out(out_file, std::ios::binary);
    CHECK(out.is_open(), "无法创建输出文件");

    size_t total_written = 0;
    for (int n = 0; n < num_frames; n++) {
        v4l2_buffer buf{};
        v4l2_plane  planes[VIDEO_MAX_PLANES]{};
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length   = VIDEO_MAX_PLANES;

        CHECK(xioctl(fd, VIDIOC_DQBUF, &buf) == 0, "VIDIOC_DQBUF (超时?)");

        size_t actual = buf.m.planes[0].bytesused;
        uint8_t *data = reinterpret_cast<uint8_t *>(buffers[buf.index].start);
        out.write(reinterpret_cast<const char *>(data), actual);
        total_written += actual;

        // 进度: 每10帧或首末帧打印
        if (n == 0 || (n + 1) % 10 == 0 || n == num_frames - 1) {
            int y_center = data[(height / 2) * width + width / 2];
            std::cout << "  [" << (n + 1) << "/" << num_frames
                      << "] bytesused=" << actual
                      << " Y(中心)=" << y_center << std::endl;
        }

        CHECK(xioctl(fd, VIDIOC_QBUF, &buf) == 0, "VIDIOC_QBUF(归还)");
    }
    out.close();
    std::cout << "已保存: " << out_file << " (" << total_written << " 字节, "
              << (total_written / frame_size) << " 帧)" << std::endl;

    // ---- 8. STREAMOFF + 清理 ----------------------------------------------
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd, VIDIOC_STREAMOFF, &type);

    for (auto &b : buffers) {
        if (b.start != nullptr && b.start != MAP_FAILED)
            munmap(b.start, b.length);
    }
    ::close(fd);

    std::cout << "=== 采集完成 ===" << std::endl;
    return 0;
}
