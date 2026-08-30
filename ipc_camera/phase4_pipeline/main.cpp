/**
 * Phase 4: 多线程实时 IPC 流水线
 *
 * 三线程架构:
 *   采集线程(V4L2) → [环形缓冲] → 编码线程(MPP) → [环形缓冲] → RTSP推流线程
 *                                       ↑
 *                            SPS/PPS 单独缓存 (供 SDP 生成)
 *
 * 编译: aarch64-rockchip-linux-gnu-g++ -std=c++17 -O2 \
 *       --sysroot=<buildroot_sysroot> \
 *       -o ipc_server main.cpp -lrockchip_mpp -lpthread
 *
 * 用法: ./ipc_server [width] [height] [fps] [port]
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <ctime>
#include <algorithm>
#include <sstream>
#include <atomic>
#include <signal.h>

#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>

extern "C" {
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_venc_cfg.h>
}

// ============================================================================
// 宏 + 常量
// ============================================================================

#define CHECK(cond, msg)                                                       \
    do { if (!(cond)) { std::cerr << "[错误] " << msg << ": "                 \
        << std::strerror(errno) << std::endl; return 1; } } while (0)

static const int  DEFAULT_PORT   = 8554;
static const int  RTP_MTU        = 1400;
static const int  RTP_PAYLOAD    = 96;
static const int  CLOCK_RATE     = 90000;
static const int  MAX_CLIENTS    = 4;
static const int  MAX_BUF        = 65536;
static const int  NV12_RING_SIZE = 16;
static const int  H264_RING_SIZE = 32;

enum RtspState { INIT, READY, PLAYING };

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_streaming{false};  // 有客户端在播放才采集编码

// ============================================================================
// 1. 线程安全环形缓冲 (支持阻塞 pop + 非阻塞 try_pop)
// ============================================================================

template<typename T>
class RingBuffer {
    std::vector<T> buf_;
    size_t head_   = 0, tail_ = 0, count_ = 0, max_;
    pthread_mutex_t mutex_  = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  cond_r_ = PTHREAD_COND_INITIALIZER;
    pthread_cond_t  cond_w_ = PTHREAD_COND_INITIALIZER;

public:
    explicit RingBuffer(size_t n) : buf_(n), max_(n) {}

    // 返回 false 表示因退出信号而放弃
    bool push(T &&item) {
        pthread_mutex_lock(&mutex_);
        while (count_ >= max_ && g_running) pthread_cond_wait(&cond_w_, &mutex_);
        if (!g_running) { pthread_mutex_unlock(&mutex_); return false; }
        buf_[head_] = std::move(item);
        head_ = (head_ + 1) % max_;
        count_++;
        pthread_cond_signal(&cond_r_);
        pthread_mutex_unlock(&mutex_);
        return true;
    }

    bool pop(T &item) {
        pthread_mutex_lock(&mutex_);
        while (count_ == 0 && g_running) pthread_cond_wait(&cond_r_, &mutex_);
        if (count_ == 0) { pthread_mutex_unlock(&mutex_); return false; }
        item = std::move(buf_[tail_]);
        tail_ = (tail_ + 1) % max_;
        count_--;
        pthread_cond_signal(&cond_w_);
        pthread_mutex_unlock(&mutex_);
        return true;
    }

    // 非阻塞取: 有数据返回 true, 无数据返回 false
    bool try_pop(T &item) {
        pthread_mutex_lock(&mutex_);
        if (count_ == 0) { pthread_mutex_unlock(&mutex_); return false; }
        item = std::move(buf_[tail_]);
        tail_ = (tail_ + 1) % max_;
        count_--;
        pthread_cond_signal(&cond_w_);
        pthread_mutex_unlock(&mutex_);
        return true;
    }

    bool empty() {
        pthread_mutex_lock(&mutex_);
        bool e = (count_ == 0);
        pthread_mutex_unlock(&mutex_);
        return e;
    }

    void wake_all() {
        pthread_cond_broadcast(&cond_r_);
        pthread_cond_broadcast(&cond_w_);
    }
};

// ============================================================================
// 2. 数据结构
// ============================================================================

struct Nv12Frame {
    std::vector<uint8_t> data;
    int64_t pts_us = 0;
};

struct H264Packet {
    std::vector<uint8_t> data;
    int64_t pts_us = 0;
    bool is_idr    = false;
};

struct RtspSession {
    int         fd       = -1;
    RtspState   state    = INIT;
    std::string sid;
    uint32_t    ssrc     = 0;
    uint16_t    seq      = 0;
    uint8_t     ch      = 0;      // RTP interleaved channel
    bool        need_idr = true;
    int64_t     start_us = 0;     // PLAY 时间
    std::string recv_buf;
};

// ============================================================================
// 3. 辅助
// ============================================================================

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR && g_running);
    return r;
}

static int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static uint8_t nal_type(const uint8_t *d, size_t len) {
    if (len >= 5 && d[0]==0 && d[1]==0 && d[2]==0 && d[3]==1) return d[4] & 0x1F;
    if (len >= 4 && d[0]==0 && d[1]==0 && d[2]==1)          return d[3] & 0x1F;
    return 0;
}

// 找 start code 长度 (3 或 4), 无 start code 返回 0
static size_t start_code_len(const uint8_t *d, size_t max_len) {
    if (max_len >= 4 && d[0]==0 && d[1]==0 && d[2]==0 && d[3]==1) return 4;
    if (max_len >= 3 && d[0]==0 && d[1]==0 && d[2]==1)          return 3;
    return 0;
}

// 扫描整个包中是否含有 IDR NAL (type 5)
static bool has_idr(const uint8_t *d, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        size_t sc = start_code_len(d + pos, len - pos);
        if (!sc) { pos++; continue; }
        if (pos + sc >= len) break;
        if ((d[pos + sc] & 0x1F) == 5) return true;
        pos += sc;
    }
    return false;
}

static std::string base64_encode(const uint8_t *data, size_t len) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    r.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i+1<len) v |= (uint32_t)data[i+1] << 8;
        if (i+2<len) v |= (uint32_t)data[i+2];
        r += t[(v>>18)&0x3F]; r += t[(v>>12)&0x3F];
        r += t[(v>>6)&0x3F];  r += t[v&0x3F];
    }
    if (len % 3) {
        size_t pad = 3 - (len % 3);
        for (size_t p = 0; p < pad; p++) r[r.size()-1-p] = '=';
    }
    return r;
}

// RTP 单包
static std::vector<uint8_t> rtp_single(const uint8_t *nal, size_t size,
                                        uint16_t seq, uint32_t ts, uint32_t ssrc,
                                        bool marker) {
    std::vector<uint8_t> pkt(12 + size, 0);
    pkt[0] = 2 << 6;
    pkt[1] = (RTP_PAYLOAD & 0x7F) | (marker ? 0x80 : 0);
    pkt[2] = (seq>>8)&0xFF;   pkt[3] = seq&0xFF;
    pkt[4] = (ts>>24)&0xFF;   pkt[5] = (ts>>16)&0xFF;
    pkt[6] = (ts>>8)&0xFF;    pkt[7] = ts&0xFF;
    pkt[8] = (ssrc>>24)&0xFF; pkt[9] = (ssrc>>16)&0xFF;
    pkt[10]=(ssrc>>8)&0xFF;   pkt[11]= ssrc&0xFF;
    memcpy(&pkt[12], nal, size);
    return pkt;
}

// RTP FU-A 分片
static std::vector<std::vector<uint8_t>>
rtp_fua(const uint8_t *nal, size_t size, uint16_t start_seq,
        uint32_t ts, uint32_t ssrc) {
    size_t maxf = RTP_MTU - 12 - 2;
    const uint8_t *frag = nal + 1;
    size_t rem = size - 1;
    size_t nf = (rem + maxf - 1) / maxf;
    std::vector<std::vector<uint8_t>> pkts;
    uint8_t nri = (nal[0]>>5)&3, orig = nal[0]&0x1F;

    for (size_t i = 0; i < nf; i++) {
        size_t fl = std::min(maxf, rem);
        std::vector<uint8_t> p(12+2+fl, 0);
        p[0]=2<<6; p[1]=(RTP_PAYLOAD&0x7F)|((i==nf-1)?0x80:0);
        uint16_t sq = start_seq + (uint16_t)i;
        p[2]=(sq>>8)&0xFF; p[3]=sq&0xFF;
        p[4]=(ts>>24)&0xFF; p[5]=(ts>>16)&0xFF;
        p[6]=(ts>>8)&0xFF;  p[7]=ts&0xFF;
        p[8]=(ssrc>>24)&0xFF; p[9]=(ssrc>>16)&0xFF;
        p[10]=(ssrc>>8)&0xFF; p[11]=ssrc&0xFF;
        p[12] = (nri<<5) | 28;
        p[13] = ((i==0)?0x80:0)|((i==nf-1)?0x40:0)|orig;
        memcpy(&p[14], frag, fl);
        pkts.push_back(std::move(p));
        frag += fl; rem -= fl;
    }
    return pkts;
}

// TCP interleaved 发送
static ssize_t send_interleaved(int fd, uint8_t ch, const std::vector<uint8_t> &pkt) {
    uint8_t hdr[4] = {'$', ch, (uint8_t)(pkt.size()>>8), (uint8_t)(pkt.size()&0xFF)};
    struct iovec iov[2] = {{hdr, 4}, {(void*)pkt.data(), pkt.size()}};
    return writev(fd, iov, 2);
}

// ============================================================================
// 4. RTSP 响应
// ============================================================================

static std::string rtsp_resp(int cseq, int code, const std::string &extra = "") {
    const char *r = (code==200)?"OK":(code==400)?"Bad Request":
        (code==454)?"Session Not Found":(code==461)?"Unsupported Transport":
        (code==405)?"Method Not Allowed":"Internal Server Error";
    std::ostringstream o;
    o << "RTSP/1.0 " << code << " " << r << "\r\n"
      << "CSeq: " << cseq << "\r\n" << extra << "\r\n";
    return o.str();
}

// ============================================================================
// 5. 采集线程
// ============================================================================

static void* capture_thread(void *arg) {
    auto *ring = static_cast<RingBuffer<Nv12Frame>*>(arg);
    int W = 640, H = 480;

    int fd = open("/dev/video0", O_RDWR);
    if (fd < 0) { std::cerr << "[采集] 无法打开 /dev/video0\n"; return nullptr; }

    // 设置格式
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width=W; fmt.fmt.pix_mp.height=H;
    fmt.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_NV12; fmt.fmt.pix_mp.field=V4L2_FIELD_ANY;
    xioctl(fd, VIDIOC_S_FMT, &fmt);
    size_t fsize = W * H * 3 / 2;
    std::cout << "[采集] " << W << "x" << H << " NV12, " << fsize << "B/帧" << std::endl;

    // MMAP
    v4l2_requestbuffers req{};
    req.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory=V4L2_MEMORY_MMAP; req.count=4;
    xioctl(fd, VIDIOC_REQBUFS, &req);

    struct Buf { void *p; size_t n; };
    std::vector<Buf> bufs(req.count);
    for (unsigned i=0;i<req.count;i++) {
        v4l2_buffer b{}; v4l2_plane pl[8]{};
        b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=V4L2_MEMORY_MMAP;
        b.index=i; b.m.planes=pl; b.length=8;
        xioctl(fd, VIDIOC_QUERYBUF, &b);
        bufs[i].n=b.m.planes[0].length;
        bufs[i].p=mmap(nullptr,b.m.planes[0].length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,b.m.planes[0].m.mem_offset);
    }

    // 入队 + STREAMON
    for (unsigned i=0;i<req.count;i++) {
        v4l2_buffer b{}; v4l2_plane pl[8]{};
        b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=V4L2_MEMORY_MMAP;
        b.index=i; b.m.planes=pl; b.length=8;
        xioctl(fd, VIDIOC_QBUF, &b);
    }
    v4l2_buf_type t=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd, VIDIOC_STREAMON, &t);

    // 丢 9 帧
    for (int i=0;i<9;i++) {
        v4l2_buffer d{}; v4l2_plane dp[8]{};
        d.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; d.memory=V4L2_MEMORY_MMAP;
        d.m.planes=dp; d.length=8;
        if (xioctl(fd, VIDIOC_DQBUF, &d)==0) xioctl(fd, VIDIOC_QBUF, &d);
    }
    std::cout << "[采集] 3A收敛完成, 开始推流" << std::endl;

    int64_t t0 = now_us(), last_t = t0;
    int cnt = 0, last_cnt = 0;
    while (g_running) {
        // poll 带超时, 避免 DQBUF 阻塞导致 Ctrl+C 无法退出
        struct pollfd pfd = {fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 1000);  // 1s 超时
        if (pr < 0) { if (errno==EINTR) continue; break; }
        if (pr == 0) continue;  // 超时, 回到 while 检查 g_running

        v4l2_buffer b{}; v4l2_plane pl[8]{};
        b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=V4L2_MEMORY_MMAP;
        b.m.planes=pl; b.length=8;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { if (errno==EINTR) continue; break; }

        xioctl(fd, VIDIOC_QBUF, &b);

        if (!g_streaming) {
            cnt++;
            continue;  // 没有客户端, 丢帧不推流
        }

        Nv12Frame nf;
        nf.data.assign((uint8_t*)bufs[b.index].p, (uint8_t*)bufs[b.index].p + b.m.planes[0].bytesused);
        nf.pts_us = now_us() - t0;
        if (!ring->push(std::move(nf))) break;  // 退出信号
        cnt++;
        if ((cnt%30)==0) {
            int64_t now = now_us();
            double fps = (cnt - last_cnt) * 1e6 / (now - last_t);
            std::cout << "[采集] " << cnt << " 帧 (" << fps << "fps)" << std::endl;
            last_t = now; last_cnt = cnt;
        }
    }

    xioctl(fd, VIDIOC_STREAMOFF, &t);
    for (auto &x:bufs) if (x.p && x.p!=MAP_FAILED) munmap(x.p, x.n);
    close(fd);
    ring->wake_all();
    std::cout << "[采集] 退出 (" << cnt << "帧)" << std::endl;
    return nullptr;
}

// ============================================================================
// 6. 编码线程 (NV12 环形缓冲 → MPP → H.264 环形缓冲)
// ============================================================================

// 编码线程同时把 SPS/PPS 存到全局变量供 RTSP 线程生成 SDP
static std::vector<uint8_t> g_sps, g_pps;
static pthread_mutex_t       g_sps_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool                  g_sps_ready = false;

// 从 MPP 输出包中提取 SPS/PPS (只存一次)
static void extract_sps_pps(const uint8_t *pd, size_t pl) {
    if (g_sps_ready) return;
    size_t pos = 0;
    while (pos < pl) {
        size_t sc = start_code_len(pd + pos, pl - pos);
        if (!sc) { pos++; continue; }
        if (pos + sc >= pl) break;
        uint8_t nt = (pd[pos + sc]) & 0x1F;
        size_t nx = pos + sc;
        bool fd = false;
        for (size_t j = nx; j < pl;) {
            size_t sc2 = start_code_len(pd + j, pl - j);
            if (sc2) { nx = j; fd = true; break; }
            j++;
        }
        if (!fd) nx = pl;
        size_t ds = nx - (pos + sc);
        if (nt == 7 && g_sps.empty()) g_sps.assign(pd + pos + sc, pd + pos + sc + ds);
        if (nt == 8 && g_pps.empty()) g_pps.assign(pd + pos + sc, pd + pos + sc + ds);
        pos = nx;
    }
    if (!g_sps.empty() && !g_pps.empty()) g_sps_ready = true;
}

static void* encode_thread(void *arg) {
    auto *rings = static_cast<std::pair<RingBuffer<Nv12Frame>*,RingBuffer<H264Packet>*>*>(arg);
    auto *in  = rings->first;
    auto *out = rings->second;
    int W=640, H=480;
    size_t fsize = W * H * 3 / 2;

    // 创建 MPP
    MppCtx ctx=nullptr; MppApi *mpi=nullptr;
    mpp_create(&ctx, &mpi);
    mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);

    MppEncCfg cfg=nullptr;
    mpp_enc_cfg_init(&cfg);
    mpp_enc_cfg_set_s32(cfg,"prep:width",W);
    mpp_enc_cfg_set_s32(cfg,"prep:height",H);
    mpp_enc_cfg_set_s32(cfg,"prep:format",MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg,"rc:mode",MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg,"rc:bps",2000000);
    mpp_enc_cfg_set_s32(cfg,"h264:gop", 30);    // 每30帧一个 IDR (后连客户端无需久等)
    mpi->control(ctx, MPP_ENC_SET_CFG, cfg);

    MppBufferGroup grp=nullptr;
    mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM);
    mpp_buffer_group_limit_config(grp, fsize, 16);  // 大池子, MPP 管线深

    std::cout << "[编码] MPP 初始化完成" << std::endl;

    int frames_fed = 0, last_fed = 0;
    int64_t last_t = 0;

    // 编码主循环: Phase 2 验证过的模式 — 逐帧处理，耐心 poll
    while (g_running) {
        // ---- 1. 取一帧 NV12 (阻塞等待) ----
        Nv12Frame nf;
        if (!in->pop(nf)) break;

        // ---- 2. 投喂给 MPP ----
        MppBuffer mb = nullptr;
        mpp_buffer_get(grp, &mb, fsize);  // 缓冲池足够大(16), 不会阻塞
        memcpy(mpp_buffer_get_ptr(mb), nf.data.data(), std::min(nf.data.size(), fsize));

        MppFrame mf = nullptr;
        mpp_frame_init(&mf);
        mpp_frame_set_width(mf, W); mpp_frame_set_height(mf, H);
        mpp_frame_set_hor_stride(mf, W); mpp_frame_set_ver_stride(mf, H);
        mpp_frame_set_fmt(mf, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(mf, mb);
        mpp_buffer_put(mb);
        mpi->encode_put_frame(ctx, mf);
        mpp_frame_deinit(&mf);

        frames_fed++;

        // ---- 3. 收割所有输出 (Phase 2 的 poll 模式) ----
        {
            int empty = 0;
            for (int i = 0; i < 60 && empty < 10; i++) {
                MppPacket pkt = nullptr;
                MPP_RET ret = mpi->encode_get_packet(ctx, &pkt);
                if (ret == MPP_OK && pkt) {
                    void *pd = mpp_packet_get_data(pkt);
                    size_t pl = mpp_packet_get_length(pkt);
                    if (pd && pl > 0) {
                        H264Packet hp;
                        hp.data.assign((uint8_t*)pd, (uint8_t*)pd + pl);
                        hp.pts_us = nf.pts_us;
                        hp.is_idr = has_idr((uint8_t*)pd, pl);

                        // 提取 SPS/PPS
                        if (!g_sps_ready) {
                            pthread_mutex_lock(&g_sps_mutex);
                            extract_sps_pps((uint8_t*)pd, pl);
                            pthread_mutex_unlock(&g_sps_mutex);
                        }

                        out->push(std::move(hp));
                    }
                    mpp_packet_deinit(&pkt);
                    empty = 0;
                } else {
                    usleep(5000);
                    empty++;
                }
            }
        }

        // ---- 4. 进度 ----
        if (frames_fed >= last_fed + 30) {
            int64_t now = now_us();
            if (last_t > 0) {
                double fps = (frames_fed - last_fed) * 1e6 / (now - last_t);
                std::cout << "[编码] " << frames_fed << " 帧 (" << fps << "fps)" << std::endl;
            } else {
                std::cout << "[编码] " << frames_fed << " 帧" << std::endl;
            }
            last_t = now; last_fed = frames_fed;
        }
    }

    // Flush — 同样不因 ret!=MPP_OK 立即 break
    mpi->encode_put_frame(ctx, nullptr);
    {
        int empty_streak = 0;
        for (int i = 0; i < 100 && empty_streak < 6; i++) {
            MppPacket pkt = nullptr;
            if (mpi->encode_get_packet(ctx, &pkt) == MPP_OK && pkt) {
                void *pd = mpp_packet_get_data(pkt);
                size_t pl = mpp_packet_get_length(pkt);
                if (pd && pl) {
                    H264Packet hp;
                    hp.data.assign((uint8_t*)pd, (uint8_t*)pd + pl);
                    hp.pts_us = -1;
                    out->push(std::move(hp));
                }
                mpp_packet_deinit(&pkt);
                empty_streak = 0;
            } else {
                usleep(10000);
                empty_streak++;
            }
        }
    }

    mpp_buffer_group_put(grp); mpp_enc_cfg_deinit(cfg);
    mpi->reset(ctx); mpp_destroy(ctx);
    out->wake_all();
    std::cout << "[编码] 退出 (投喂" << frames_fed << "帧)" << std::endl;
    return nullptr;
}

// ============================================================================
// 7. SDP 生成
// ============================================================================

static std::string make_sdp(const char *ip) {
    std::string sps_b64, pps_b64, sprop;
    pthread_mutex_lock(&g_sps_mutex);
    if (!g_sps.empty()) sps_b64 = base64_encode(g_sps.data(), g_sps.size());
    if (!g_pps.empty()) pps_b64 = base64_encode(g_pps.data(), g_pps.size());
    pthread_mutex_unlock(&g_sps_mutex);
    if (!sps_b64.empty()) sprop = sps_b64 + "," + pps_b64;

    std::ostringstream s;
    s << "v=0\r\n";
    s << "o=- " << time(nullptr) << " 1 IN IP4 " << ip << "\r\n";
    s << "s=RK3568 Live\r\n";
    s << "c=IN IP4 " << ip << "\r\n";
    s << "t=0 0\r\n";
    s << "m=video 0 RTP/AVP " << RTP_PAYLOAD << "\r\n";
    s << "a=rtpmap:" << RTP_PAYLOAD << " H264/" << CLOCK_RATE << "\r\n";
    s << "a=framerate:30\r\n";
    if (!sprop.empty())
        s << "a=fmtp:" << RTP_PAYLOAD << " packetization-mode=1;"
          << " sprop-parameter-sets=" << sprop << "\r\n";
    s << "a=control:track0\r\n";
    return s.str();
}

// ============================================================================
// 8. 发送一个 H.264 包给客户端 (NAL 解析 + RTP 打包 + TCP interleaved)
// ============================================================================

static void send_h264_to_client(RtspSession &sess, const H264Packet &hp,
                                 uint32_t rtp_ts) {
    // 等待 IDR
    if (sess.need_idr) {
        if (!hp.is_idr) return;

        // 先发送缓存的 SPS + PPS (新客户端解码必需)
        pthread_mutex_lock(&g_sps_mutex);
        std::vector<uint8_t> sps = g_sps, pps = g_pps;  // 拷贝
        pthread_mutex_unlock(&g_sps_mutex);

        auto send_nal = [&](const std::vector<uint8_t> &nal) {
            if (nal.empty()) return;
            if (nal.size() <= (size_t)RTP_MTU - 12) {
                auto pkt = rtp_single(nal.data(), nal.size(), sess.seq++, rtp_ts, sess.ssrc, false);
                send_interleaved(sess.fd, sess.ch, pkt);
            } else {
                auto frags = rtp_fua(nal.data(), nal.size(), sess.seq, rtp_ts, sess.ssrc);
                sess.seq += (uint16_t)frags.size();
                for (auto &fk : frags)
                    send_interleaved(sess.fd, sess.ch, fk);
            }
        };

        send_nal(sps);
        send_nal(pps);

        sess.need_idr = false;
        sess.start_us = now_us();
        std::cout << "  [client:" << sess.fd << "] SPS/PPS+IDR 就绪, 开始推流" << std::endl;
    }

    const uint8_t *d = hp.data.data();
    size_t len = hp.data.size();
    size_t pos = 0;

    while (pos < len) {
        size_t sc = start_code_len(d + pos, len - pos);
        if (!sc) { pos++; continue; }

        // 找下一个 start code
        size_t next = pos + sc;
        bool found = false;
        for (size_t j = next; j < len; ) {
            size_t sc2 = start_code_len(d + j, len - j);
            if (sc2) { next = j; found = true; break; }
            j++;
        }
        if (!found) next = len;

        size_t nal_start = pos + sc;
        size_t nal_size  = next - nal_start;
        if (nal_size == 0) { pos = next; continue; }

        uint8_t nt = d[nal_start] & 0x1F;
        bool is_last_nal = (next >= len);
        bool marker = is_last_nal;  // M-bit: 帧最后一个 NAL

        if (nt == 9) { pos = next; continue; }  // 跳过 AUD

        if (nal_size <= (size_t)RTP_MTU - 12) {
            auto pkt = rtp_single(d + nal_start, nal_size, sess.seq++, rtp_ts, sess.ssrc, marker);
            send_interleaved(sess.fd, sess.ch, pkt);
        } else {
            auto frags = rtp_fua(d + nal_start, nal_size, sess.seq, rtp_ts, sess.ssrc);
            sess.seq += (uint16_t)frags.size();
            for (auto &fk : frags)
                send_interleaved(sess.fd, sess.ch, fk);
        }
        pos = next;
    }
}

// ============================================================================
// 9. RTSP 请求处理
// ============================================================================

static void handle_rtsp(RtspSession &sess, const std::string &req,
                         const std::string &sdp) {
    std::istringstream is(req);
    std::string method, url, ver;
    is >> method >> url >> ver;

    int cseq = 0;
    size_t p = req.find("CSeq:");
    if (p != std::string::npos) cseq = std::stoi(req.substr(p+5));

    std::string rsession;
    p = req.find("Session:");
    if (p != std::string::npos) {
        size_t e = req.find("\r\n", p);
        rsession = req.substr(p+8, e-p-8);
        while (!rsession.empty() && rsession[0]==' ') rsession.erase(0,1);
    }

    std::string resp;

    if (method == "OPTIONS") {
        resp = rtsp_resp(cseq, 200, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n");

    } else if (method == "DESCRIBE") {
        resp = rtsp_resp(cseq, 200,
            "Content-Type: application/sdp\r\nContent-Length: "
            + std::to_string(sdp.size()) + "\r\n") + sdp;

    } else if (method == "SETUP") {
        sess.sid  = std::to_string((uint64_t)sess.fd)+"_"+std::to_string(now_us());
        sess.state= READY;
        sess.seq  = (uint16_t)(now_us()&0xFFFF);
        sess.ssrc = (uint32_t)(now_us()&0xFFFFFFFF);
        sess.ch   = 0;

        if (req.find("RTP/AVP/TCP") == std::string::npos) {
            resp = rtsp_resp(cseq, 461, "");  // 只接收 TCP
        } else {
            std::ostringstream h;
            h << "Session: " << sess.sid << "\r\n"
              << "Transport: RTP/AVP/TCP;unicast;interleaved="
              << (int)sess.ch << "-" << (int)(sess.ch+1) << ";mode=play\r\n";
            resp = rtsp_resp(cseq, 200, h.str());
        }

    } else if (method == "PLAY") {
        if (rsession != sess.sid) { resp = rtsp_resp(cseq, 454, ""); }
        else {
            sess.state = PLAYING;
            sess.need_idr = true;
            std::ostringstream h;
            h << "Session: " << sess.sid << "\r\n"
              << "Range: npt=0.000-\r\n";
            resp = rtsp_resp(cseq, 200, h.str());
        }

    } else if (method == "TEARDOWN") {
        sess.state = INIT;
        resp = rtsp_resp(cseq, 200, "Session: " + sess.sid + "\r\n");
    } else {
        resp = rtsp_resp(cseq, 405, "");
    }

    // 尝试一次性发送 (忽略 short write, 简化)
    send(sess.fd, resp.data(), resp.size(), MSG_NOSIGNAL);
}

// ============================================================================
// 10. RTSP 服务线程
// ============================================================================

static void* rtsp_thread(void *arg) {
    auto *h264_ring = static_cast<RingBuffer<H264Packet>*>(arg);

    // ---- TCP 监听 ----
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { std::cerr << "[RTSP] socket 失败\n"; return nullptr; }
    int opt=1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa{};
    sa.sin_family=AF_INET; sa.sin_addr.s_addr=INADDR_ANY;
    sa.sin_port=htons(DEFAULT_PORT);
    bind(lfd, (sockaddr*)&sa, sizeof(sa));
    listen(lfd, MAX_CLIENTS);

    // 获取本机 IP
    char ip[64]="192.168.0.200";
    { int ts=socket(AF_INET,SOCK_DGRAM,0);
        if(ts>=0){ sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(1);
            inet_pton(AF_INET,"1.1.1.1",&a.sin_addr);
            if(connect(ts,(sockaddr*)&a,sizeof(a))==0){ sockaddr_in la{}; socklen_t ln=sizeof(la);
                getsockname(ts,(sockaddr*)&la,&ln); inet_ntop(AF_INET,&la.sin_addr,ip,sizeof(ip)); }
            close(ts); } }

    // 等待 SPS/PPS (最多 3 秒)
    std::cout << "[RTSP] 等待 SPS/PPS (最多3秒)..." << std::endl;
    for (int w=0; w<30 && g_running && !g_sps_ready; w++)
        usleep(100000);  // 100ms

    std::string sdp = make_sdp(ip);
    if (g_sps_ready)
        std::cout << "[RTSP] SDP 就绪 (SPS=" << g_sps.size() << "B PPS=" << g_pps.size() << "B)" << std::endl;
    else
        std::cout << "[RTSP] ⚠️  SPS/PPS 超时, SDP 不含 sprop" << std::endl;

    std::cout << "[RTSP] 监听 rtsp://" << ip << ":" << DEFAULT_PORT << "/stream" << std::endl;

    std::vector<RtspSession> clients;

    // 用于给每帧分配连续的 RTP timestamp
    int64_t rtp_ts_counter = 0;
    int64_t last_frame_pts = -1;

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        int maxfd = lfd;
        for (auto &c : clients) { FD_SET(c.fd, &rfds); if (c.fd>maxfd) maxfd=c.fd; }

        struct timeval tv{0, 50000};  // 50ms (足够频繁)
        select(maxfd+1, &rfds, nullptr, nullptr, &tv);

        // ---- 消费 H.264 环形缓冲 (非阻塞) ----
        H264Packet hp;
        while (h264_ring->try_pop(hp)) {
            // 每帧分配新的 RTP timestamp
            if (hp.pts_us != last_frame_pts) {
                rtp_ts_counter += CLOCK_RATE / 30;  // 90000/30 = 3000
                last_frame_pts = hp.pts_us;
            }

            // 推送给所有 PLAYING 客户端
            for (auto &c : clients) {
                if (c.state == PLAYING) {
                    send_h264_to_client(c, hp, (uint32_t)rtp_ts_counter);
                }
            }
        }

        // ---- 接受新连接 ----
        if (FD_ISSET(lfd, &rfds)) {
            sockaddr_in ca{}; socklen_t cl=sizeof(ca);
            int cf=accept(lfd, (sockaddr*)&ca, &cl);
            if (cf>=0) {
                int flag=1; setsockopt(cf, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
                char cip[64]; inet_ntop(AF_INET, &ca.sin_addr, cip, sizeof(cip));
                std::cout << "[RTSP] 连接: " << cip << ":" << ntohs(ca.sin_port) << std::endl;
                clients.push_back(RtspSession{cf, INIT});
            }
        }

        // ---- 处理客户端 I/O ----
        for (auto it=clients.begin(); it!=clients.end(); ) {
            auto &c = *it;
            bool dead = false;

            if (FD_ISSET(c.fd, &rfds)) {
                char buf[MAX_BUF];
                ssize_t n=recv(c.fd, buf, sizeof(buf)-1, 0);
                if (n>0) {
                    buf[n]=0; c.recv_buf+=buf;
                    size_t ep=c.recv_buf.find("\r\n\r\n");
                    if (ep!=std::string::npos) {
                        std::string req=c.recv_buf.substr(0, ep+4);
                        c.recv_buf.erase(0, ep+4);
                        std::string fl=req.substr(0, req.find("\r\n"));
                        std::cout << "[RTSP] " << fl << std::endl;
                        handle_rtsp(c, req, sdp);
                        if (fl.find("TEARDOWN")!=std::string::npos)
                            { close(c.fd); dead=true; std::cout << "[RTSP] 断开" << std::endl; }
                    }
                } else if (n==0||(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK))
                    { close(c.fd); dead=true; std::cout << "[RTSP] 断开" << std::endl; }
            }

            if (dead) { it=clients.erase(it); }
            else ++it;
        }

        // 更新 g_streaming: 有任一客户端在 PLAYING 就推流
        {
            bool any = false;
            for (auto &c : clients) if (c.state == PLAYING) any = true;
            g_streaming = any;
        }
    }

    close(lfd);
    for (auto &c:clients) close(c.fd);
    std::cout << "[RTSP] 退出" << std::endl;
    return nullptr;
}

// ============================================================================
// 11. main
// ============================================================================

static RingBuffer<Nv12Frame>   *g_nv12 = nullptr;
static RingBuffer<H264Packet>  *g_h264 = nullptr;

static void sig_handler(int) {
    std::cout << "\n[信号] 收到退出信号, 停止中..." << std::endl;
    g_running = false;
    if (g_nv12) g_nv12->wake_all();
    if (g_h264) g_h264->wake_all();
}

int main(int argc, char *argv[]) {
    int W = (argc>1)?std::stoi(argv[1]):640, H = (argc>2)?std::stoi(argv[2]):480;
    int fps=(argc>3)?std::stoi(argv[3]):30, port=(argc>4)?std::stoi(argv[4]):8554;

    std::cout << "=== RK3568 实时 IPC 流水线 (Phase 4) ===" << std::endl;
    std::cout << W << "x" << H << " @" << fps << "fps, 端口 " << port << std::endl;

    signal(SIGINT, sig_handler); signal(SIGTERM, sig_handler);

    RingBuffer<Nv12Frame>   nv12_ring(NV12_RING_SIZE);
    RingBuffer<H264Packet>  h264_ring(H264_RING_SIZE);
    g_nv12=&nv12_ring; g_h264=&h264_ring;

    auto enc_args = std::make_pair(&nv12_ring, &h264_ring);

    pthread_t ct, et, rt;
    pthread_create(&ct, nullptr, capture_thread, &nv12_ring);
    pthread_create(&et, nullptr, encode_thread,  &enc_args);
    pthread_create(&rt, nullptr, rtsp_thread,    &h264_ring);

    std::cout << "\n📺 rtsp://192.168.0.200:" << port << "/stream" << std::endl;
    std::cout << "   Ctrl+C 停止\n" << std::endl;

    pthread_join(ct, nullptr);
    pthread_join(et, nullptr);
    pthread_join(rt, nullptr);

    std::cout << "=== Phase 4 退出 ===" << std::endl;
    return 0;
}
