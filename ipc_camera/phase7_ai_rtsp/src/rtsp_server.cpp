/**
 * rtsp_server.cpp — RTSP/RTP 服务器实现
 *
 * 复用:
 *   - Phase 3: RTSP 状态机, RTP 打包 (Single NAL + FU-A), TCP interleaved
 *   - Phase 5: select() 事件循环, 非阻塞发送, 多客户端管理
 *
 * 新设计:
 *   - 从 RingBuffer<H264Packet> 消费 (而非文件)
 *   - SPS/PPS 从 SharedState 缓存读取 (而非预解析)
 *   - 新客户端: 缓存 SPS→PPS→等 IDR→开始推流
 */

#include "rtsp_server.h"

#include <iostream>
#include <sstream>
#include <cstring>
#include <cerrno>
#include <ctime>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

// ---- 常量 ----
static const int    RTSP_PORT      = 8554;
static const int    RTP_MTU        = 1400;
static const int    RTP_PAYLOAD    = 96;
static const int    CLOCK_RATE     = 90000;
static const int    MAX_CLIENTS    = 4;
static const int    MAX_BUF        = 65536;

enum RtspState { INIT, READY, PLAYING };

// ---- 辅助: 时间 ----
static int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

// ---- NAL 解析辅助 ----
static size_t start_code_len(const uint8_t* d, size_t max_len) {
    if (max_len >= 4 && d[0]==0 && d[1]==0 && d[2]==0 && d[3]==1) return 4;
    if (max_len >= 3 && d[0]==0 && d[1]==0 && d[2]==1)          return 3;
    return 0;
}

// ---- Base64 编码 (SDP sprop-parameter-sets) ----
static std::string base64_encode(const uint8_t* data, size_t len) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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

// ═══════════════════════════════════════════════════════════════════
// RTP 打包 (RFC 6184, 从 Phase 3 复用)
// ═══════════════════════════════════════════════════════════════════

static std::vector<uint8_t> rtp_single(const uint8_t* nal, size_t size,
                                        uint16_t seq, uint32_t ts,
                                        uint32_t ssrc, bool marker) {
    std::vector<uint8_t> pkt(12 + size, 0);
    pkt[0] = 2 << 6;
    pkt[1] = (uint8_t)(RTP_PAYLOAD & 0x7F) | (marker ? 0x80 : 0);
    pkt[2] = (seq>>8)&0xFF;   pkt[3] = seq&0xFF;
    pkt[4] = (ts>>24)&0xFF;   pkt[5] = (ts>>16)&0xFF;
    pkt[6] = (ts>>8)&0xFF;    pkt[7] = ts&0xFF;
    pkt[8] = (ssrc>>24)&0xFF; pkt[9] = (ssrc>>16)&0xFF;
    pkt[10]=(ssrc>>8)&0xFF;   pkt[11]=ssrc&0xFF;
    memcpy(&pkt[12], nal, size);
    return pkt;
}

static std::vector<std::vector<uint8_t>>
rtp_fua(const uint8_t* nal, size_t size, uint16_t start_seq,
        uint32_t ts, uint32_t ssrc) {
    size_t maxf = RTP_MTU - 12 - 2;
    const uint8_t* frag = nal + 1;
    size_t rem = size - 1;
    size_t nf = (rem + maxf - 1) / maxf;
    std::vector<std::vector<uint8_t>> pkts;
    uint8_t nri = (nal[0]>>5)&3, orig = nal[0] & 0x1F;

    for (size_t i = 0; i < nf; i++) {
        size_t fl = std::min(maxf, rem);
        std::vector<uint8_t> p(12+2+fl, 0);
        p[0]=2<<6;
        p[1]=(uint8_t)(RTP_PAYLOAD&0x7F)|((i==nf-1)?0x80:0);
        uint16_t sq = start_seq + (uint16_t)i;
        p[2]=(sq>>8)&0xFF; p[3]=sq&0xFF;
        p[4]=(ts>>24)&0xFF; p[5]=(ts>>16)&0xFF;
        p[6]=(ts>>8)&0xFF;  p[7]=ts&0xFF;
        p[8]=(ssrc>>24)&0xFF; p[9]=(ssrc>>16)&0xFF;
        p[10]=(ssrc>>8)&0xFF; p[11]=ssrc&0xFF;
        p[12] = (nri<<5) | 28;
        p[13] = ((i==0)?0x80:0) | ((i==nf-1)?0x40:0) | orig;
        memcpy(&p[14], frag, fl);
        pkts.push_back(std::move(p));
        frag += fl; rem -= fl;
    }
    return pkts;
}

// TCP interleaved 发送
static ssize_t send_interleaved(int fd, uint8_t ch,
                                 const std::vector<uint8_t>& pkt) {
    uint8_t hdr[4] = {'$', ch,
                      (uint8_t)(pkt.size()>>8),
                      (uint8_t)(pkt.size()&0xFF)};
    struct iovec iov[2] = {{hdr, 4}, {(void*)pkt.data(), pkt.size()}};
    return writev(fd, iov, 2);
}

// ============================================================================
// RTSP 会话
// ============================================================================

struct RtspSession {
    int         fd       = -1;
    RtspState   state    = INIT;
    std::string sid;
    uint32_t    ssrc     = 0;
    uint16_t    seq      = 0;
    uint8_t     ch       = 0;       // RTP interleaved channel
    bool        need_idr = true;    // 新客户端需等待 IDR
    int64_t     start_us = 0;
    std::string recv_buf;
};

// ============================================================================
// RTSP 响应
// ============================================================================

static std::string rtsp_resp(int cseq, int code,
                              const std::string& extra = "") {
    const char* r = (code==200)?"OK":(code==400)?"Bad Request":
        (code==454)?"Session Not Found":(code==461)?"Unsupported Transport":
        (code==405)?"Method Not Allowed":"Internal Server Error";
    std::ostringstream o;
    o << "RTSP/1.0 " << code << " " << r << "\r\n"
      << "CSeq: " << cseq << "\r\n" << extra << "\r\n";
    return o.str();
}

// ============================================================================
// SDP 生成 (从 SharedState.sps/pps 缓存读取)
// ============================================================================

static std::string make_sdp(SharedState* s, const char* ip) {
    std::string sps_b64, pps_b64, sprop;

    pthread_mutex_lock(&s->sps_mutex);
    if (!s->sps.empty()) sps_b64 = base64_encode(s->sps.data(), s->sps.size());
    if (!s->pps.empty()) pps_b64 = base64_encode(s->pps.data(), s->pps.size());
    pthread_mutex_unlock(&s->sps_mutex);

    if (!sps_b64.empty()) sprop = sps_b64 + "," + pps_b64;

    std::ostringstream o;
    o << "v=0\r\n";
    o << "o=- " << time(nullptr) << " 1 IN IP4 " << ip << "\r\n";
    o << "s=RK3568 Live\r\n";
    o << "c=IN IP4 " << ip << "\r\n";
    o << "t=0 0\r\n";
    o << "m=video 0 RTP/AVP " << RTP_PAYLOAD << "\r\n";
    o << "a=rtpmap:" << RTP_PAYLOAD << " H264/" << CLOCK_RATE << "\r\n";
    o << "a=framerate:30\r\n";
    if (!sprop.empty())
        o << "a=fmtp:" << RTP_PAYLOAD << " packetization-mode=1;"
          << " sprop-parameter-sets=" << sprop << "\r\n";
    o << "a=control:track0\r\n";
    return o.str();
}

// ============================================================================
// 发送一个 H.264 包给客户端 (NAL解析 + RTP打包)
// ============================================================================

static void send_h264_to_client(RtspSession& sess, const H264Packet& hp,
                                 uint32_t rtp_ts) {
    // 等待 IDR
    if (sess.need_idr) {
        if (!hp.is_idr) return;
        sess.need_idr = false;
        sess.start_us = now_us();
        std::cout << "  [client:" << sess.fd << "] IDR 就绪, 开始推流"
                  << std::endl;
    }

    const uint8_t* d = hp.data.data();
    size_t len = hp.data.size();
    size_t pos = 0;

    while (pos < len) {
        size_t sc = start_code_len(d + pos, len - pos);
        if (!sc) { pos++; continue; }

        size_t next = pos + sc;
        bool found = false;
        for (size_t j = next; j < len;) {
            size_t sc2 = start_code_len(d + j, len - j);
            if (sc2) { next = j; found = true; break; }
            j++;
        }
        if (!found) next = len;

        size_t nal_start = pos + sc;
        size_t nal_size  = next - nal_start;
        if (nal_size == 0) { pos = next; continue; }

        uint8_t nt = d[nal_start] & 0x1F;
        bool marker = (next >= len);

        if (nt == 9 || nt == 6) { pos = next; continue; } // 跳过 AUD/SEI

        if (nal_size <= (size_t)RTP_MTU - 12) {
            auto pkt = rtp_single(d + nal_start, nal_size,
                                  sess.seq++, rtp_ts, sess.ssrc, marker);
            send_interleaved(sess.fd, sess.ch, pkt);
        } else {
            auto frags = rtp_fua(d + nal_start, nal_size,
                                 sess.seq, rtp_ts, sess.ssrc);
            sess.seq += (uint16_t)frags.size();
            for (auto& fk : frags)
                send_interleaved(sess.fd, sess.ch, fk);
        }
        pos = next;
    }
}

// ============================================================================
// RTSP 请求处理
// ============================================================================

static void handle_rtsp(RtspSession& sess, const std::string& req,
                         SharedState* s, const std::string& sdp) {
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
        resp = rtsp_resp(cseq, 200,
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n");

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
            resp = rtsp_resp(cseq, 461);
        } else {
            std::ostringstream h;
            h << "Session: " << sess.sid << "\r\n"
              << "Transport: RTP/AVP/TCP;unicast;interleaved="
              << (int)sess.ch << "-" << (int)(sess.ch+1) << ";mode=play\r\n";
            resp = rtsp_resp(cseq, 200, h.str());
        }

    } else if (method == "PLAY") {
        if (rsession != sess.sid) { resp = rtsp_resp(cseq, 454); }
        else {
            sess.state = PLAYING;
            sess.need_idr = true;  // 等下一个 IDR 才开始发
            std::ostringstream h;
            h << "Session: " << sess.sid << "\r\n"
              << "Range: npt=0.000-\r\n";
            resp = rtsp_resp(cseq, 200, h.str());
        }

    } else if (method == "TEARDOWN") {
        sess.state = INIT;
        resp = rtsp_resp(cseq, 200, "Session: " + sess.sid + "\r\n");
    } else {
        resp = rtsp_resp(cseq, 405);
    }

    send(sess.fd, resp.data(), resp.size(), MSG_NOSIGNAL);
}

// ============================================================================
// 向新客户端发送缓存的 SPS/PPS (在 PLAY 之后、第一个 IDR 之前)
// ============================================================================

static void inject_sps_pps(RtspSession& sess, SharedState* s,
                            uint32_t rtp_ts) {
    pthread_mutex_lock(&s->sps_mutex);
    std::vector<uint8_t> sps_copy = s->sps;
    std::vector<uint8_t> pps_copy = s->pps;
    pthread_mutex_unlock(&s->sps_mutex);

    auto send_nal = [&](const std::vector<uint8_t>& nal) {
        if (nal.empty()) return;
        if (nal.size() <= (size_t)RTP_MTU - 12) {
            auto pkt = rtp_single(nal.data(), nal.size(),
                                  sess.seq++, rtp_ts, sess.ssrc, false);
            send_interleaved(sess.fd, sess.ch, pkt);
        } else {
            auto frags = rtp_fua(nal.data(), nal.size(),
                                 sess.seq, rtp_ts, sess.ssrc);
            sess.seq += (uint16_t)frags.size();
            for (auto& fk : frags)
                send_interleaved(sess.fd, sess.ch, fk);
        }
    };

    send_nal(sps_copy);
    send_nal(pps_copy);

    std::cout << "  [client:" << sess.fd << "] SPS/PPS 已注入" << std::endl;
}

// ============================================================================
// RTSP 服务器线程
// ============================================================================

void* rtsp_thread(void* arg) {
    SharedState* s = static_cast<SharedState*>(arg);

    // ---- 1. TCP 监听 ----
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        std::cerr << "[RTSP] ❌ socket 失败" << std::endl;
        return nullptr;
    }
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port        = htons(RTSP_PORT);
    bind(lfd, (sockaddr*)&sa, sizeof(sa));
    listen(lfd, MAX_CLIENTS);

    // 获取本机 IP
    char ip[64] = "192.168.0.200";
    {
        int ts = socket(AF_INET, SOCK_DGRAM, 0);
        if (ts >= 0) {
            sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(1);
            inet_pton(AF_INET, "1.1.1.1", &a.sin_addr);
            if (connect(ts, (sockaddr*)&a, sizeof(a)) == 0) {
                sockaddr_in la{}; socklen_t ln=sizeof(la);
                getsockname(ts, (sockaddr*)&la, &ln);
                inet_ntop(AF_INET, &la.sin_addr, ip, sizeof(ip));
            }
            close(ts);
        }
    }

    // ---- 2. 等待 SPS/PPS (最多5秒) ----
    std::cout << "[RTSP] 等待 SPS/PPS..." << std::endl;
    for (int w = 0; w < 50 && s->running && !s->sps_pps_ready; w++)
        usleep(100000);

    if (s->sps_pps_ready) {
        std::cout << "[RTSP] SDP 就绪 (SPS=" << s->sps.size()
                  << "B PPS=" << s->pps.size() << "B)" << std::endl;
    } else {
        std::cout << "[RTSP] ⚠️ SPS/PPS 超时, SDP 不含 sprop (仍然可工作)"
                  << std::endl;
    }

    std::string sdp = make_sdp(s, ip);
    std::cout << "[RTSP] 监听 rtsp://" << ip << ":" << RTSP_PORT
              << "/stream" << std::endl;

    // ---- 3. select() 事件循环 ----
    std::vector<RtspSession> clients;
    int64_t last_pts  = -1;
    int     frame_seq = 0;

    while (s->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        int maxfd = lfd;
        for (auto& c : clients) {
            FD_SET(c.fd, &rfds);
            if (c.fd > maxfd) maxfd = c.fd;
        }

        // 33ms 超时 (~30fps)
        struct timeval tv = {0, 33000};
        select(maxfd + 1, &rfds, nullptr, nullptr, &tv);

        // ---- 3a. 消费 H.264 环, 推流给 PLAYING 客户端 ----
        H264Packet hp;
        while (s->h264_ring->try_pop(hp)) {
            // 分配 RTP timestamp
            uint32_t rtp_ts;
            if (hp.pts_us != last_pts) {
                frame_seq++;
                rtp_ts = (uint32_t)(frame_seq * CLOCK_RATE / 30);
                last_pts = hp.pts_us;
            } else {
                rtp_ts = (uint32_t)(frame_seq * CLOCK_RATE / 30);
            }

            for (auto& c : clients) {
                if (c.state != PLAYING) continue;

                // 新客户端首次: 先注入 SPS/PPS
                if (c.need_idr && s->sps_pps_ready) {
                    inject_sps_pps(c, s, rtp_ts);
                }

                send_h264_to_client(c, hp, rtp_ts);
            }
        }

        // ---- 3b. 接受新连接 ----
        if (FD_ISSET(lfd, &rfds)) {
            sockaddr_in ca{}; socklen_t cl = sizeof(ca);
            int cf = accept(lfd, (sockaddr*)&ca, &cl);
            if (cf >= 0) {
                int flag = 1;
                setsockopt(cf, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
                char cip[64];
                inet_ntop(AF_INET, &ca.sin_addr, cip, sizeof(cip));
                std::cout << "[RTSP] 连接: " << cip << ":"
                          << ntohs(ca.sin_port) << std::endl;
                clients.push_back(RtspSession{cf, INIT});
            }
        }

        // ---- 3c. 客户端 I/O ----
        for (auto it = clients.begin(); it != clients.end(); ) {
            RtspSession& c = *it;
            bool dead = false;

            if (FD_ISSET(c.fd, &rfds)) {
                char buf[MAX_BUF];
                ssize_t n = recv(c.fd, buf, sizeof(buf)-1, 0);
                if (n > 0) {
                    buf[n] = 0;
                    c.recv_buf += buf;
                    size_t ep = c.recv_buf.find("\r\n\r\n");
                    if (ep != std::string::npos) {
                        std::string req = c.recv_buf.substr(0, ep+4);
                        c.recv_buf.erase(0, ep+4);
                        std::string fl = req.substr(0, req.find("\r\n"));
                        std::cout << "[RTSP] " << fl << std::endl;
                        handle_rtsp(c, req, s, sdp);
                        if (fl.find("TEARDOWN") != std::string::npos)
                            dead = true;
                    }
                } else if (n == 0 ||
                           (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    dead = true;
                }
            }

            if (dead) {
                std::cout << "[RTSP] 断开 fd=" << c.fd << std::endl;
                close(c.fd);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---- 4. 清理 ----
    for (auto& c : clients) close(c.fd);
    close(lfd);
    std::cout << "[RTSP] 退出" << std::endl;
    return nullptr;
}
