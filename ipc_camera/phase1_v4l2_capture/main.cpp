/**
 * Phase 1: V4L2 摄像头采集 + 保存 NV12 原始帧
 *
 * 流程: 打开设备 → 设置格式 → 申请缓冲区(MMAP) → 开始采集
 *       → 取一帧 → 保存为 .yuv 文件 → 清理退出
 *
 * 编译: aarch64-rockchip-linux-gnu-g++ -std=c++17 -O2 -o capture main.cpp
 * 用法: ./capture [/dev/video0] [output.yuv] [width] [height]
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

// ---------------------------------------------------------------------------
// 辅助: 打印错误并退出
// ---------------------------------------------------------------------------
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "[错误] " << msg << ": " << std::strerror(errno)      \
                      << std::endl;                                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// 辅助: ioctl 带重试 (被信号打断时重试)
// ---------------------------------------------------------------------------
static int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

int main(int argc, char *argv[]) {
    // ---- 参数解析 ----------------------------------------------------------
    const char *dev_name = (argc > 1) ? argv[1] : "/dev/video0";
    const char *out_file = (argc > 2) ? argv[2] : "capture.yuv";
    int req_width        = (argc > 3) ? std::stoi(argv[3]) : 640;
    int req_height       = (argc > 4) ? std::stoi(argv[4]) : 480;

    std::cout << "=== RK3568 V4L2 摄像头采集 (Phase 1) ===" << std::endl;
    std::cout << "设备: " << dev_name << std::endl;
    std::cout << "分辨率: " << req_width << "x" << req_height << std::endl;
    std::cout << "输出文件: " << out_file << std::endl;

    // ---- 1. 打开设备 -------------------------------------------------------
    int fd = ::open(dev_name, O_RDWR);
    CHECK(fd >= 0, "无法打开设备");

    // ---- 2. 查询设备能力 ---------------------------------------------------
    v4l2_capability cap{};
    CHECK(xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0, "VIDIOC_QUERYCAP");
    std::cout << "驱动: " << cap.driver << std::endl;
    std::cout << "设备: " << cap.card  << std::endl;
    std::cout << "总线: " << cap.bus_info << std::endl;
    CHECK(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE,
          "不是采集设备(非 Multiplanar)");
    CHECK(cap.capabilities & V4L2_CAP_STREAMING, "不支持流式采集");

    // ---- 3. 设置采集格式 (NV12) --------------------------------------------
    v4l2_format fmt{};
    fmt.type                  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width      = static_cast<__u32>(req_width);
    fmt.fmt.pix_mp.height     = static_cast<__u32>(req_height);
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field      = V4L2_FIELD_ANY;
    CHECK(xioctl(fd, VIDIOC_S_FMT, &fmt) == 0, "VIDIOC_S_FMT(NV12)");

    int width  = fmt.fmt.pix_mp.width;
    int height = fmt.fmt.pix_mp.height;
    std::cout << "实际分辨率: " << width << "x" << height << std::endl;

    // NV12 单帧大小 = Y平面(W*H) + UV交叠平面(W*H/2)
    size_t frame_size = width * height * 3 / 2;
    std::cout << "帧大小: " << frame_size << " 字节 (NV12)" << std::endl;

    // ---- 4. 申请缓冲区 (MMAP方式) ------------------------------------------
    v4l2_requestbuffers req{};
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count  = 4;
    CHECK(xioctl(fd, VIDIOC_REQBUFS, &req) == 0, "VIDIOC_REQBUFS");
    CHECK(req.count >= 2, "缓冲区不足");
    std::cout << "申请了 " << req.count << " 个缓冲区" << std::endl;

    // ---- 5. 查询每个 buffer 并 mmap 到用户空间 -----------------------------
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

    // ---- 6. 所有 buffer 入队，开始采集 -------------------------------------
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
    std::cout << "采集已启动，等待 ISP 3A 收敛 (丢掉前9帧)..." << std::endl;

    // 丢掉前 9 帧，让 3A 算法有时间调整曝光和增益
    for (int i = 0; i < 9; i++) {
        v4l2_buffer drop{};
        v4l2_plane  dp[VIDEO_MAX_PLANES]{};
        drop.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        drop.memory   = V4L2_MEMORY_MMAP;
        drop.m.planes = dp;
        drop.length   = VIDEO_MAX_PLANES;
        if (xioctl(fd, VIDIOC_DQBUF, &drop) == 0) {
            xioctl(fd, VIDIOC_QBUF, &drop);
        }
    }

    // ---- 7. 取第10帧 -------------------------------------------------------
    v4l2_buffer buf{};
    v4l2_plane  planes[VIDEO_MAX_PLANES]{};

    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length   = VIDEO_MAX_PLANES;

    CHECK(xioctl(fd, VIDIOC_DQBUF, &buf) == 0, "VIDIOC_DQBUF (取帧超时?)");
    std::cout << "取到一帧, index=" << buf.index
              << ", bytesused=" << buf.m.planes[0].bytesused
              << " (预期 " << frame_size << ")" << std::endl;

    // 打印几个 Y 分量采样点，判断画面亮度
    uint8_t *y_plane = reinterpret_cast<uint8_t *>(buffers[buf.index].start);
    std::cout << "Y 采样: (0,0)=" << static_cast<int>(y_plane[0])
              << " (10,10)=" << static_cast<int>(y_plane[10 * width + 10])
              << " (中心)=" << static_cast<int>(y_plane[(height/2) * width + width/2])
              << " (正常户外50~180, 太暗<20)" << std::endl;

    // ---- 8. 保存到文件 -----------------------------------------------------
    {
        std::ofstream out(out_file, std::ios::binary);
        CHECK(out.is_open(), "无法创建输出文件");

        size_t actual = buf.m.planes[0].bytesused;
        out.write(reinterpret_cast<const char *>(y_plane), actual);
        out.close();

        std::cout << "已保存: " << out_file << " (" << actual << " 字节)" << std::endl;
    }

    // ---- 9. 停止采集，归还缓冲区 -------------------------------------------
    CHECK(xioctl(fd, VIDIOC_QBUF, &buf) == 0, "VIDIOC_QBUF(归还)");

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    CHECK(xioctl(fd, VIDIOC_STREAMOFF, &type) == 0, "VIDIOC_STREAMOFF");

    // ---- 10. 清理 ----------------------------------------------------------
    for (auto &b : buffers) {
        if (b.start != nullptr && b.start != MAP_FAILED)
            munmap(b.start, b.length);
    }
    ::close(fd);

    std::cout << "=== Phase 1 完成 ===" << std::endl;
    std::cout << "提示: PC 上运行 ffplay -f rawvideo -pixel_format nv12 "
         << "-video_size " << width << "x" << height
         << " " << out_file << " 可预览" << std::endl;

    return 0;
}
