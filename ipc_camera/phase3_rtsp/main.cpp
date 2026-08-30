/**
 * Phase 3: RTSP H.264 推流服务器
 *
 * 手写 RTSP/RTP 协议栈，零外部依赖。仅用 POSIX socket API。
 *
 * 协议层次:
 *   应用层   RTSP (RFC 2326)  — 文本协议, 类 HTTP
 *   传输层   RTP over TCP interleaved (RFC 2326 §10.12)
 *   打包层   H.264 over RTP (RFC 6184) — Single NAL + FU-A
 *
 * 整体流程:
 *   1. 加载 .h264 文件 → 解析 NAL units → 提取 SPS/PPS 做 SDP
 *   2. TCP 监听 8554 端口
 *   3. select() 事件循环: 接新连接 + 处理 RTSP + 定时发送 RTP
 *
 * 编译: aarch64-rockchip-linux-gnu-g++ -std=c++17 -O2 -o rtsp_server main.cpp
 * 用法: ./rtsp_server [input.h264] [port]
 *
 * 客户端播放:
 *   ffplay rtsp://192.168.0.200:8554/stream
 *   vlc    rtsp://192.168.0.200:8554/stream
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

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

// ============================================================================
// 宏
// ============================================================================

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "[错误] " << msg << ": " << std::strerror(errno)      \
                      << std::endl;                                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// ============================================================================
// 常量
// ============================================================================

static const int    RTP_PORT        = 8554;        // TCP 监听端口
static const int    RTP_MTU         = 1400;        // RTP 最大载荷 (留余量给 TCP interleaved 头)
static const int    RTP_PAYLOAD     = 96;           // H.264 dynamic payload type
static const int    CLOCK_RATE      = 90000;        // H.264 RTP 时钟频率
static const int    FPS             = 30;           // 默认帧率
static const int    MAX_CLIENTS     = 4;            // 最大并发客户端
static const int    MAX_BUF         = 65536;        // 接收缓冲区

// RTSP 状态机
enum RtspState { INIT, READY, PLAYING };

// ============================================================================
// 1. NAL Unit 解析器
// ============================================================================

// 连续扫描 start code 找到的单个 NAL unit
struct NalUnit {
    size_t offset;      // 在文件中的起始偏移 (指 start code 的第一个 0x00)
    size_t data_offset; // NAL 数据起始 (跳过 start code)
    size_t size;        // NAL 数据字节数 (不含 start code)
    uint8_t type;       // nal_unit_type (低5位)
    bool    is_slice;   // 是否是 VCL NAL (IDR / non-IDR slice)
};

// 从 .h264 Annex B 码流中解析所有 NAL units
// 支持 4字节 (0x00000001) 和 3字节 (0x000001) start code
static std::vector<NalUnit> parse_nal_units(const std::vector<uint8_t> &data) {
    std::vector<NalUnit> nals;
    size_t i = 0;
    const size_t len = data.size();

    while (i < len) {
        // 找 start code
        size_t sc_len = 0;
        if (i + 4 <= len &&
            data[i]==0x00 && data[i+1]==0x00 && data[i+2]==0x00 && data[i+3]==0x01) {
            sc_len = 4;
        } else if (i + 3 <= len &&
                   data[i]==0x00 && data[i+1]==0x00 && data[i+2]==0x01) {
            sc_len = 3;
        }

        if (sc_len > 0) {
            // 找下一个 start code 作为当前 NAL 的结束
            size_t next = i + sc_len;
            bool found = false;

            // 从 next 开始搜下一个 start code
            for (size_t j = next; j < len; ) {
                if (j + 4 <= len &&
                    data[j]==0x00 && data[j+1]==0x00 && data[j+2]==0x00 && data[j+3]==0x01) {
                    next = j; found = true; break;
                }
                if (j + 3 <= len &&
                    data[j]==0x00 && data[j+1]==0x00 && data[j+2]==0x01) {
                    next = j; found = true; break;
                }
                j++;
            }
            if (!found) next = len;

            size_t data_start = i + sc_len;
            size_t data_size  = next - data_start;

            if (data_size > 0) {
                NalUnit nal;
                nal.offset      = i;
                nal.data_offset = data_start;
                nal.size        = data_size;
                nal.type        = data[data_start] & 0x1F;
                nal.is_slice    = (nal.type == 1 || nal.type == 5);
                nals.push_back(nal);
            }

            i = next;
        } else {
            i++;
        }
    }
    return nals;
}

// 找 NAL units 中的 SPS 和 PPS (用于 SDP)
static const NalUnit* find_nal(const std::vector<NalUnit> &nals, uint8_t type) {
    for (auto &n : nals)
        if (n.type == type) return &n;
    return nullptr;
}

// ============================================================================
// 2. RTP 打包器 (RFC 6184)
// ============================================================================

// 打 RTP 包 (Single NAL unit 模式)
// 返回 RTP packet (含 12 字节头)
static std::vector<uint8_t> rtp_single(const NalUnit &nal,
                                        const std::vector<uint8_t> &data,
                                        uint16_t seq, uint32_t ts, uint32_t ssrc,
                                        bool marker) {
    size_t pkt_size = 12 + nal.size;
    std::vector<uint8_t> pkt(pkt_size, 0);

    // RTP 头
    pkt[0] = (2 << 6);                              // V=2
    pkt[1] = (uint8_t)(RTP_PAYLOAD & 0x7F) | (marker ? 0x80 : 0x00);  // PT + M
    pkt[2] = (seq >> 8) & 0xFF;
    pkt[3] = seq & 0xFF;
    pkt[4] = (ts >> 24) & 0xFF;
    pkt[5] = (ts >> 16) & 0xFF;
    pkt[6] = (ts >> 8)  & 0xFF;
    pkt[7] = ts & 0xFF;
    pkt[8] = (ssrc >> 24) & 0xFF;
    pkt[9] = (ssrc >> 16) & 0xFF;
    pkt[10]= (ssrc >> 8)  & 0xFF;
    pkt[11]= ssrc & 0xFF;

    // NAL 数据 (去掉 start code, 直接拷贝 NAL unit data)
    memcpy(pkt.data() + 12, data.data() + nal.data_offset, nal.size);
    return pkt;
}

// 打 FU-A 分片包
// 返回多个 RTP packet
static std::vector<std::vector<uint8_t>>
rtp_fua(const NalUnit &nal, const std::vector<uint8_t> &data,
        uint16_t start_seq, uint32_t ts, uint32_t ssrc) {

    const uint8_t *nal_data = data.data() + nal.data_offset;
    size_t nal_size = nal.size;

    // FU-A 载荷: 去掉 NAL header (1字节), 每片最多 (MTU - 12 - 2) 字节
    size_t max_frag = RTP_MTU - 12 - 2;
    const uint8_t *frag_start = nal_data + 1;  // 跳过 NAL header
    size_t frag_remaining = nal_size - 1;

    // 计算片数
    size_t num_frags = (frag_remaining + max_frag - 1) / max_frag;
    std::vector<std::vector<uint8_t>> packets;

    uint8_t nri  = (nal_data[0] >> 5) & 0x03;    // 从原始 NAL header 取 NRI
    uint8_t orig = nal_data[0] & 0x1F;            // 原始 NAL type

    for (size_t i = 0; i < num_frags; i++) {
        size_t frag_len = std::min(max_frag, frag_remaining);
        size_t pkt_size = 12 + 2 + frag_len;
        std::vector<uint8_t> pkt(pkt_size, 0);

        bool first = (i == 0);
        bool last  = (i == num_frags - 1);

        // RTP 头
        pkt[0] = (2 << 6);
        pkt[1] = (uint8_t)(RTP_PAYLOAD & 0x7F) | (last ? 0x80 : 0x00); // M 只在最后一包
        uint16_t seq = start_seq + (uint16_t)i;
        pkt[2] = (seq >> 8) & 0xFF;
        pkt[3] = seq & 0xFF;
        pkt[4] = (ts >> 24) & 0xFF;
        pkt[5] = (ts >> 16) & 0xFF;
        pkt[6] = (ts >> 8)  & 0xFF;
        pkt[7] = ts & 0xFF;
        pkt[8] = (ssrc >> 24) & 0xFF;
        pkt[9] = (ssrc >> 16) & 0xFF;
        pkt[10]= (ssrc >> 8)  & 0xFF;
        pkt[11]= ssrc & 0xFF;

        // FU indicator
        pkt[12] = (0 << 7) | (nri << 5) | 28;     // F=0, NRI=nri, Type=28(FU-A)

        // FU header
        pkt[13] = (first ? 0x80 : 0x00)            // S
                | (last  ? 0x40 : 0x00)            // E
                | orig;                             // 原始 NAL type

        // FU payload
        memcpy(pkt.data() + 14, frag_start, frag_len);

        packets.push_back(std::move(pkt));
        frag_start      += frag_len;
        frag_remaining  -= frag_len;
    }
    return packets;
}

// ============================================================================
// 3. H.264 帧流水线
// ============================================================================

// 把 NAL units 划分为帧 (VCL NAL + 其前面的 non-VCL NAL 定义为一帧)
// 简化: 每个 VCL NAL + 其前面的 SPS/PPS/SEI = 一个可解码帧
struct H264Frame {
    std::vector<size_t> nal_indices;   // 指向 nals[] 的索引
    bool    is_idr;                     // 是否包含 IDR
    int64_t pts;                        // 微秒 (理论播放时间)
};

static std::vector<H264Frame> build_frames(const std::vector<NalUnit> &nals, int fps) {
    std::vector<H264Frame> frames;
    H264Frame cur;
    cur.is_idr = false;
    cur.pts    = 0;
    int64_t frame_interval = 1000000LL / fps;  // 微秒

    for (size_t i = 0; i < nals.size(); i++) {
        cur.nal_indices.push_back(i);

        if (nals[i].type == 5) cur.is_idr = true;

        // VCL NAL (IDR 5, non-IDR 1) 标志帧结束
        if (nals[i].is_slice) {
            cur.pts = (int64_t)frames.size() * frame_interval;
            frames.push_back(cur);
            cur = H264Frame{};
            cur.is_idr = false;
        }
    }
    // 最后可能有没有 slice 的 trailing NALs, 丢弃
    return frames;
}

// ============================================================================
// 4. SDP 生成器
// ============================================================================

static std::string base64_encode(const uint8_t *data, size_t len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    r.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i+1] << 8;
        if (i + 2 < len) v |= (uint32_t)data[i+2];
        r += tbl[(v >> 18) & 0x3F];
        r += tbl[(v >> 12) & 0x3F];
        r += tbl[(v >> 6)  & 0x3F];
        r += tbl[v & 0x3F];
    }
    // 如果 len%3 != 0, 替换尾部为 =
    if (len % 3 != 0) {
        size_t pad = 3 - (len % 3);
        for (size_t p = 0; p < pad; p++)
            r[r.size() - 1 - p] = '=';
    }
    return r;
}

static std::string generate_sdp(const std::vector<uint8_t> &raw_data,
                                 const NalUnit *sps, const NalUnit *pps,
                                 const char *server_ip) {
    std::string sprop;
    if (sps && pps) {
        std::string b64_sps = base64_encode(raw_data.data() + sps->data_offset, sps->size);
        std::string b64_pps = base64_encode(raw_data.data() + pps->data_offset, pps->size);
        sprop = b64_sps + "," + b64_pps;
    }

    std::ostringstream s;
    s << "v=0\r\n";
    s << "o=- " << time(nullptr) << " 1 IN IP4 " << server_ip << "\r\n";
    s << "s=RK3568 IPC Stream\r\n";
    s << "c=IN IP4 " << server_ip << "\r\n";
    s << "t=0 0\r\n";
    s << "m=video 0 RTP/AVP " << RTP_PAYLOAD << "\r\n";
    s << "a=rtpmap:" << RTP_PAYLOAD << " H264/" << CLOCK_RATE << "\r\n";
    s << "a=framerate:" << FPS << "\r\n";
    if (!sprop.empty())
        s << "a=fmtp:" << RTP_PAYLOAD << " packetization-mode=1;"
          << " sprop-parameter-sets=" << sprop << "\r\n";
    s << "a=control:track0\r\n";
    return s.str();
}

// ============================================================================
// 5. RTSP 会话
// ============================================================================

struct RtspSession {
    int         client_fd;
    RtspState   state;
    std::string session_id;
    uint32_t    ssrc;           // 本会话的 RTP SSRC
    uint16_t    rtp_seq;        // 下一个 RTP 序列号
    uint32_t    rtp_ts_base;    // 起始时间戳 (发送第一帧时设置)
    uint16_t    rtp_channel;    // TCP interleaved channel (0=RTP, 1=RTCP for video)
    size_t      frame_idx;      // 当前发送到第几帧
    int64_t     stream_start_us;// 流开始时间 (gettimeofday)
    int64_t     frame_pts_base; // 首帧 pts (用于平移)
    std::string recv_buf;       // 接收缓冲区 (拼 RTSP 请求)

    RtspSession() : client_fd(-1), state(INIT), ssrc(0), rtp_seq(0),
                    rtp_ts_base(0), rtp_channel(0), frame_idx(0),
                    stream_start_us(0), frame_pts_base(-1) {}
};

// ============================================================================
// 6. 辅助: 时间
// ============================================================================

static int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

// ============================================================================
// 7. RTSP 消息构建
// ============================================================================

static std::string rtsp_response(int cseq, int code, const std::string &extra_headers = "") {
    std::ostringstream r;
    const char *reason = (code == 200) ? "OK" :
                         (code == 400) ? "Bad Request" :
                         (code == 404) ? "Not Found" :
                         (code == 405) ? "Method Not Allowed" :
                         (code == 454) ? "Session Not Found" :
                         (code == 461) ? "Unsupported Transport" : "Internal Server Error";
    r << "RTSP/1.0 " << code << " " << reason << "\r\n";
    r << "CSeq: " << cseq << "\r\n";
    if (!extra_headers.empty())
        r << extra_headers;
    r << "\r\n";
    return r.str();
}

// ============================================================================
// 8. 发送 TCP interleaved RTP
// ============================================================================

// 格式: $ + channel(1B) + len(2B network) + data
static ssize_t send_rtp_interleaved(int fd, uint8_t channel,
                                     const std::vector<uint8_t> &rtp_pkt) {
    uint8_t header[4];
    header[0] = '$';
    header[1] = channel;
    header[2] = (rtp_pkt.size() >> 8) & 0xFF;
    header[3] = rtp_pkt.size() & 0xFF;

    // 一次 send 可能不完整, 用 writev 或者两次 send
    // 简化: 拼接后一次发
    std::vector<uint8_t> frame(4 + rtp_pkt.size());
    memcpy(frame.data(), header, 4);
    memcpy(frame.data() + 4, rtp_pkt.data(), rtp_pkt.size());

    ssize_t sent = send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    return sent;
}

// ============================================================================
// 9. RTSP 请求处理器
// ============================================================================

static void handle_rtsp(RtspSession &sess, const std::string &request,
                         const std::string &sdp,
                         const std::vector<H264Frame> &frames,
                         const std::vector<NalUnit> &nals,
                         const std::vector<uint8_t> &raw_data) {

    // 解析首行: METHOD url RTSP/1.0
    std::istringstream is(request);
    std::string method, url, version;
    is >> method >> url >> version;

    // 解析 CSeq
    // 简单查找 "CSeq: N"
    int cseq = 0;
    {
        size_t p = request.find("CSeq:");
        if (p != std::string::npos) {
            cseq = std::stoi(request.substr(p + 5));
        }
    }

    // 解析 Session (如果有)
    std::string req_session;
    {
        size_t p = request.find("Session:");
        if (p != std::string::npos) {
            size_t e = request.find("\r\n", p);
            req_session = request.substr(p + 8, e - p - 8);
            // trim
            while (!req_session.empty() && req_session[0] == ' ') req_session.erase(0, 1);
        }
    }

    std::string response;

    if (method == "OPTIONS") {
        response = rtsp_response(cseq, 200,
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n");

    } else if (method == "DESCRIBE") {
        std::string body = sdp;
        std::ostringstream hdr;
        hdr << "Content-Type: application/sdp\r\n";
        hdr << "Content-Length: " << body.size() << "\r\n";
        response = rtsp_response(cseq, 200, hdr.str()) + body;

    } else if (method == "SETUP") {
        // 生成 session ID
        sess.session_id = std::to_string((uint64_t)sess.client_fd) + "_"
                        + std::to_string(now_us());
        sess.state     = READY;
        sess.rtp_seq   = (uint16_t)(now_us() & 0xFFFF);
        sess.ssrc      = (uint32_t)(now_us() & 0xFFFFFFFF);
        sess.rtp_channel = 0;  // RTP on channel 0, RTCP on 1

        // 只支持 TCP interleaved
        size_t tp = request.find("RTP/AVP/TCP");
        if (tp == std::string::npos) {
            response = rtsp_response(cseq, 461, "");
            return;
        }

        std::ostringstream hdr;
        hdr << "Session: " << sess.session_id << "\r\n";
        hdr << "Transport: RTP/AVP/TCP;unicast;"
            << "interleaved=" << (int)sess.rtp_channel << "-" << (int)(sess.rtp_channel + 1)
            << ";mode=play\r\n";
        response = rtsp_response(cseq, 200, hdr.str());

    } else if (method == "PLAY") {
        if (req_session != sess.session_id) {
            response = rtsp_response(cseq, 454, "");
            return;
        }
        sess.state = PLAYING;
        sess.frame_idx      = 0;
        sess.stream_start_us = now_us();
        sess.frame_pts_base  = -1;  // 首帧时设置

        std::ostringstream hdr;
        hdr << "Session: " << sess.session_id << "\r\n";
        hdr << "Range: npt=0.000-\r\n";
        response = rtsp_response(cseq, 200, hdr.str());

    } else if (method == "TEARDOWN") {
        sess.state = INIT;
        response = rtsp_response(cseq, 200,
            "Session: " + sess.session_id + "\r\n");

    } else {
        response = rtsp_response(cseq, 405, "");
    }

    send(sess.client_fd, response.data(), response.size(), MSG_NOSIGNAL);
}

// ============================================================================
// 10. 定时发送帧
// ============================================================================

static void send_pending_frames(RtspSession &sess,
                                 const std::vector<H264Frame> &frames,
                                 const std::vector<NalUnit> &nals,
                                 const std::vector<uint8_t> &raw_data) {
    if (sess.state != PLAYING) return;
    if (sess.frame_idx >= frames.size()) {
        // 循环播放: 回到开头
        sess.frame_idx = 0;
        sess.stream_start_us = now_us();
        sess.frame_pts_base  = -1;
    }

    int64_t now = now_us();
    int64_t elapsed = now - sess.stream_start_us;

    // 找到应该发送的帧
    while (sess.frame_idx < frames.size()) {
        const H264Frame &f = frames[sess.frame_idx];

        // 首帧设置 PTS base
        if (sess.frame_pts_base < 0)
            sess.frame_pts_base = f.pts;

        int64_t target_time = f.pts - sess.frame_pts_base;

        // 还没到发送时间
        if (target_time > elapsed) return;

        // 发送这一帧的所有 NAL units
        int64_t rtp_ts = sess.rtp_ts_base +
                         (int64_t)((f.pts - sess.frame_pts_base) * CLOCK_RATE / 1000000LL);

        for (size_t ni : f.nal_indices) {
            const NalUnit &nal = nals[ni];
            bool last_nal = (&nal == &nals[f.nal_indices.back()]);

            // 跳过 AUD (type 9) 等非必要 NAL
            if (nal.type == 9 || nal.type == 6) {
                // SEI (6) 也发, 但 AUD 不发
                if (nal.type == 9) continue;
            }

            if (nal.size <= (size_t)RTP_MTU - 12) {
                // 单包模式
                auto pkt = rtp_single(nal, raw_data, sess.rtp_seq,
                                      (uint32_t)rtp_ts, sess.ssrc, last_nal ? true : false);
                send_rtp_interleaved(sess.client_fd, sess.rtp_channel, pkt);
                sess.rtp_seq++;
            } else {
                // FU-A 分片模式
                auto frags = rtp_fua(nal, raw_data, sess.rtp_seq,
                                     (uint32_t)rtp_ts, sess.ssrc);
                for (size_t k = 0; k < frags.size(); k++) {
                    // TODO: 按 RTP_MTU + interleaved 头 限制大小
                    send_rtp_interleaved(sess.client_fd, sess.rtp_channel, frags[k]);
                }
                sess.rtp_seq += (uint16_t)frags.size();
            }
        }
        sess.frame_idx++;
    }
}

// ============================================================================
// 11. main
// ============================================================================

int main(int argc, char *argv[]) {
    const char *h264_file = (argc > 1) ? argv[1] : "capture_30.h264";
    int         port      = (argc > 2) ? std::stoi(argv[2]) : 8554;

    std::cout << "=== RK3568 RTSP H.264 推流服务器 (Phase 3) ===" << std::endl;
    std::cout << "输入: " << h264_file << ", 端口: " << port << std::endl;

    // ---- 1. 加载 .h264 文件 ------------------------------------------------
    std::ifstream fin(h264_file, std::ios::binary | std::ios::ate);
    CHECK(fin.is_open(), "无法打开 H.264 文件");
    size_t fsize = fin.tellg();
    fin.seekg(0);
    CHECK(fsize > 0, "H.264 文件为空");

    std::vector<uint8_t> raw_data(fsize);
    fin.read(reinterpret_cast<char *>(raw_data.data()), fsize);
    fin.close();
    std::cout << "[1] 加载文件: " << fsize << " 字节" << std::endl;

    // ---- 2. 解析 NAL units ------------------------------------------------
    auto nals = parse_nal_units(raw_data);
    std::cout << "[2] 解析 NAL: " << nals.size() << " 个 units" << std::endl;

    const NalUnit *sps = find_nal(nals, 7);
    const NalUnit *pps = find_nal(nals, 8);
    if (!sps || !pps) {
        std::cerr << "[警告] 未找到 SPS/PPS, SDP 将不包含 sprop-parameter-sets" << std::endl;
    } else {
        std::cout << "  SPS: " << sps->size << "B, PPS: " << pps->size << "B" << std::endl;
    }

    // ---- 3. 构建帧 ---------------------------------------------------------
    auto frames = build_frames(nals, FPS);
    std::cout << "[3] 构建帧: " << frames.size() << " 帧 (FPS=" << FPS << ")" << std::endl;
    if (frames.empty()) {
        std::cerr << "[错误] 文件中没有可解码的视频帧 (VCL NAL)" << std::endl;
        return 1;
    }
    // 首帧检查
    if (frames[0].is_idr)
        std::cout << "  首帧为 IDR 关键帧 ✅" << std::endl;

    // ---- 4. 生成 SDP -------------------------------------------------------
    // 获取本机 IP (通过 socket 连接到外部获取, 而非实际发送)
    char server_ip[64] = "192.168.0.200";
    {
        int tmp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (tmp_sock >= 0) {
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(1);
            inet_pton(AF_INET, "1.1.1.1", &addr.sin_addr);
            if (connect(tmp_sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                struct sockaddr_in local{};
                socklen_t len = sizeof(local);
                getsockname(tmp_sock, (struct sockaddr *)&local, &len);
                inet_ntop(AF_INET, &local.sin_addr, server_ip, sizeof(server_ip));
            }
            close(tmp_sock);
        }
    }
    std::string sdp = generate_sdp(raw_data, sps, pps, server_ip);
    std::cout << "[4] SDP 已生成 (IP=" << server_ip << ")" << std::endl;

    // ---- 5. 创建 TCP 监听 socket ------------------------------------------
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_fd >= 0, "socket");

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv_addr{};
    srv_addr.sin_family      = AF_INET;
    srv_addr.sin_addr.s_addr = INADDR_ANY;
    srv_addr.sin_port        = htons((uint16_t)port);

    CHECK(bind(listen_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) == 0, "bind");
    CHECK(listen(listen_fd, MAX_CLIENTS) == 0, "listen");

    std::cout << "[5] 监听: rtsp://" << server_ip << ":" << port << "/stream" << std::endl;
    std::cout << "\n📺 客户端播放命令:" << std::endl;
    std::cout << "   ffplay rtsp://" << server_ip << ":" << port << "/stream" << std::endl;
    std::cout << "   vlc    rtsp://" << server_ip << ":" << port << "/stream" << std::endl;
    std::cout << "\n等待客户端连接...\n" << std::endl;

    // ---- 6. select() 主循环 -----------------------------------------------
    std::vector<RtspSession> sessions;

    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int max_fd = listen_fd;

        for (auto &s : sessions) {
            FD_SET(s.client_fd, &rfds);
            if (s.client_fd > max_fd) max_fd = s.client_fd;
        }

        // 超时: 帧间隔的 1/3 (用于定时发送)
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 1000000 / FPS / 3;  // ~11ms for 30fps

        int ret = select(max_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[错误] select: " << std::strerror(errno) << std::endl;
            break;
        }

        // 接受新连接
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in cli_addr{};
            socklen_t cli_len = sizeof(cli_addr);
            int cli_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
            if (cli_fd >= 0) {
                // 设置 TCP_NODELAY (降低延迟)
                int flag = 1;
                setsockopt(cli_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                char cli_ip[64];
                inet_ntop(AF_INET, &cli_addr.sin_addr, cli_ip, sizeof(cli_ip));
                std::cout << "[连接] " << cli_ip << ":" << ntohs(cli_addr.sin_port) << std::endl;

                RtspSession sess;
                sess.client_fd = cli_fd;
                sessions.push_back(sess);
            }
        }

        // 处理客户端数据 + 定时发送
        for (auto it = sessions.begin(); it != sessions.end(); ) {
            RtspSession &sess = *it;
            bool disconnected = false;

            // 定时发送帧 (PLAYING 状态)
            send_pending_frames(sess, frames, nals, raw_data);

            // 读取 RTSP 请求
            if (FD_ISSET(sess.client_fd, &rfds)) {
                char buf[MAX_BUF];
                ssize_t n = recv(sess.client_fd, buf, sizeof(buf) - 1, 0);
                if (n > 0) {
                    buf[n] = '\0';
                    sess.recv_buf += buf;

                    // 检查是否收到完整 RTSP 请求 (以 \r\n\r\n 结束)
                    size_t end_pos = sess.recv_buf.find("\r\n\r\n");
                    if (end_pos != std::string::npos) {
                        std::string request = sess.recv_buf.substr(0, end_pos + 4);
                        sess.recv_buf.erase(0, end_pos + 4);

                        // 打印请求摘要
                        std::string first_line = request.substr(0, request.find("\r\n"));
                        std::cout << "[RTSP] " << first_line << std::endl;

                        handle_rtsp(sess, request, sdp, frames, nals, raw_data);

                        // TEARDOWN 后断开
                        if (first_line.find("TEARDOWN") != std::string::npos) {
                            std::cout << "[断开] TEARDOWN, fd=" << sess.client_fd << std::endl;
                            close(sess.client_fd);
                            disconnected = true;
                        }
                    }
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    // 客户端断开
                    std::cout << "[断开] fd=" << sess.client_fd << std::endl;
                    close(sess.client_fd);
                    disconnected = true;
                }
            }

            if (disconnected) {
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---- 7. 清理 -----------------------------------------------------------
    close(listen_fd);
    for (auto &s : sessions) close(s.client_fd);

    std::cout << "=== Phase 3 服务器退出 ===" << std::endl;
    return 0;
}
