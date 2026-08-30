/**
 * http_server.cpp — HTTP MJPEG 服务器实现
 *
 * 设计要点 (对比 Phase 4):
 *   - 无 RTSP/RTP/FU-A, 纯 HTTP multipart
 *   - select() 同时监控可读和可写 fd (防止慢客户端阻塞)
 *   - writev() 单系统调用发送 MIME 边界 + JPEG 数据
 *   - 单槽位模型: 消费最新的 JPEG 帧即可
 */

#include "http_server.h"

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cerrno>
#include <sstream>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

// ---- 常量 ----
static const int    LISTEN_PORT    = 8080;
static const int    MAX_CLIENTS    = 16;
static const int    MAX_HEADER     = 8192;
#define MJPEG_BOUNDARY "frame"

// ---- HTTP MJPEG 响应头模板 ----
static const char HTTP_200_HEADER[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
    "Cache-Control: no-cache, no-store, must-revalidate\r\n"
    "Pragma: no-cache\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char HTTP_404_BODY[] =
    "HTTP/1.0 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "\r\n"
    "404 Not Found";

static const char HTTP_500_BODY[] =
    "HTTP/1.0 500 Internal Server Error\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 21\r\n"
    "\r\n"
    "500 Internal Error";

// ---- MJPEG 帧边界前缀 ----
static const char MJPEG_PART_PREFIX[] =
    "--" MJPEG_BOUNDARY "\r\n"
    "Content-Type: image/jpeg\r\n"
    "Content-Length: ";
// 后接 ASCII 长度 + "\r\n\r\n" + JPEG bytes + "\r\n"

// ---- 客户端状态 ----
struct Client {
    int         fd;
    int         state;      // 0=reading request, 1=streaming
    std::string recv_buf;
    uint64_t    last_seq;   // 上次发送的帧序号 (去重)
};

// ---- 辅助 ----
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ---- 发送 MJPEG 响应头 ----
static bool send_mjpeg_header(int fd) {
    size_t len = sizeof(HTTP_200_HEADER) - 1; // exclude null terminator
    ssize_t n = send(fd, HTTP_200_HEADER, len, MSG_NOSIGNAL);
    return n == (ssize_t)len;
}

// ---- 发送单帧 JPEG 作为 MJPEG part ----
// 返回 true 表示发送成功, false 表示应该断开该客户端
static bool send_mjpeg_frame(int fd, const JpegFrame& jpeg) {
    // 构建 Content-Length 字符串
    char len_str[32];
    int len_n = snprintf(len_str, sizeof(len_str), "%zu", jpeg.data.size());

    // 使用 writev 单系统调用发送: 边界头 + Content-Length + \r\n\r\n + JPEG + \r\n
    struct iovec iov[6];
    iov[0].iov_base = (void*)MJPEG_PART_PREFIX;
    iov[0].iov_len  = sizeof(MJPEG_PART_PREFIX) - 1;
    iov[1].iov_base = len_str;
    iov[1].iov_len  = (size_t)len_n;
    iov[2].iov_base = (void*)"\r\n\r\n";
    iov[2].iov_len  = 4;
    iov[3].iov_base = (void*)jpeg.data.data();
    iov[3].iov_len  = jpeg.data.size();
    iov[4].iov_base = (void*)"\r\n";
    iov[4].iov_len  = 2;

    ssize_t total = 0;
    for (int i = 0; i < 5; i++) total += iov[i].iov_len;

    // 非阻塞发送: 如果一次 send 发不完, 简单丢弃 (MJPEG 容忍丢帧)
    ssize_t sent = writev(fd, iov, 5);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;   // 缓冲区满, 跳过本帧, 不认为客户端断开
        return false;      // 真正的错误 (EPIPE, ECONNRESET 等)
    }
    // sent < total 也表示发不完整, 但 MJPEG 可以容忍
    return (sent > 0);
}

// ---- 发送快照 (单帧 JPEG, Content-Disposition) ----
static void send_snapshot(int fd, const JpegFrame& jpeg) {
    char header[512];
    int hl = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        jpeg.data.size());

    struct iovec iov[2];
    iov[0].iov_base = header;
    iov[0].iov_len  = (size_t)hl;
    iov[1].iov_base = (void*)jpeg.data.data();
    iov[1].iov_len  = jpeg.data.size();

    writev(fd, iov, 2);
}

// ═══════════════════════════════════════════════════════════════════════
// HTTP 服务器线程
// ═══════════════════════════════════════════════════════════════════════

void* http_server_thread(void* arg) {
    SharedState* s = static_cast<SharedState*>(arg);

    // ---- 1. 创建监听 socket ----
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        std::cerr << "[HTTP] ❌ socket 失败: " << std::strerror(errno) << std::endl;
        return nullptr;
    }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port        = htons(LISTEN_PORT);

    if (bind(lfd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        std::cerr << "[HTTP] ❌ bind 失败: " << std::strerror(errno) << std::endl;
        close(lfd); return nullptr;
    }
    if (listen(lfd, MAX_CLIENTS) < 0) {
        std::cerr << "[HTTP] ❌ listen 失败: " << std::strerror(errno) << std::endl;
        close(lfd); return nullptr;
    }

    std::cout << "[HTTP] 监听 http://0.0.0.0:" << LISTEN_PORT << "/" << std::endl;

    std::vector<Client> clients;
    JpegFrame local_jpeg;
    uint64_t  last_sent_seq = 0;
    int       sent_count    = 0;

    // ---- 2. select() 事件循环 ----
    while (s->running) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds); FD_ZERO(&wfds);
        FD_SET(lfd, &rfds);
        int maxfd = lfd;

        for (auto& c : clients) {
            FD_SET(c.fd, &rfds);
            if (c.state == 1) {
                // 只对 streaming 客户端监控可写性
                FD_SET(c.fd, &wfds);
            }
            if (c.fd > maxfd) maxfd = c.fd;
        }

        // 33ms 超时 (~30fps), 也用于定期检查 running 标志
        struct timeval tv = {0, 33000};
        int ret = select(maxfd + 1, &rfds, &wfds, nullptr, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[HTTP] ❌ select 错误: " << std::strerror(errno) << std::endl;
            break;
        }

        // ---- 2a. 拷贝最新 JPEG (在 select 之后立即做) ----
        pthread_mutex_lock(&s->jpeg_mutex);
        bool has_new = (s->jpeg.seq != last_sent_seq);
        if (has_new) {
            local_jpeg.data.swap(s->jpeg.data);
            local_jpeg.pts_us = s->jpeg.pts_us;
            last_sent_seq = s->jpeg.seq;
        }
        pthread_mutex_unlock(&s->jpeg_mutex);

        // ---- 2b. 接受新连接 ----
        if (FD_ISSET(lfd, &rfds)) {
            struct sockaddr_in ca{};
            socklen_t cl = sizeof(ca);
            int cf = accept(lfd, (struct sockaddr*)&ca, &cl);

            if (cf >= 0 && (int)clients.size() < MAX_CLIENTS) {
                set_nonblock(cf);
                int flag = 1;
                setsockopt(cf, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                char cip[64];
                inet_ntop(AF_INET, &ca.sin_addr, cip, sizeof(cip));

                std::cout << "[HTTP] 连接: " << cip << ":"
                          << ntohs(ca.sin_port) << std::endl;

                clients.push_back(Client{cf, 0, "", 0});
            } else if (cf >= 0) {
                close(cf); // 达到客户端上限
            }
        }

        // ---- 2c. 处理客户端 I/O ----
        for (auto it = clients.begin(); it != clients.end(); ) {
            Client& c = *it;
            bool dead = false;

            // 读取 HTTP 请求
            if (FD_ISSET(c.fd, &rfds) && c.state == 0) {
                char buf[4096];
                ssize_t n = recv(c.fd, buf, sizeof(buf) - 1, 0);
                if (n > 0) {
                    buf[n] = '\0';
                    c.recv_buf += buf;

                    // 检测请求结束
                    if (c.recv_buf.find("\r\n\r\n") != std::string::npos) {
                        // 解析请求行
                        std::string first_line = c.recv_buf.substr(0, c.recv_buf.find("\r\n"));
                        std::cout << "[HTTP] " << first_line << std::endl;

                        if (first_line.find("GET /snapshot") != std::string::npos) {
                            // 快照模式: 发送一帧后关闭
                            if (local_jpeg.data.empty()) {
                                send(c.fd, HTTP_500_BODY, sizeof(HTTP_500_BODY)-1, MSG_NOSIGNAL);
                            } else {
                                send_snapshot(c.fd, local_jpeg);
                            }
                            dead = true;
                        } else if (first_line.find("GET") != std::string::npos) {
                            // MJPEG 流模式
                            if (send_mjpeg_header(c.fd)) {
                                c.state = 1; // 切换到 streaming
                                c.last_seq = 0;
                                std::cout << "  → MJPEG streaming" << std::endl;
                            } else {
                                dead = true;
                            }
                        } else {
                            // 不支持的请求
                            send(c.fd, HTTP_404_BODY, sizeof(HTTP_404_BODY)-1, MSG_NOSIGNAL);
                            dead = true;
                        }
                    }
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    dead = true;
                }
            }

            // 发送 MJPEG 帧 (streaming 状态 + 有新帧 + fd 可写)
            if (c.state == 1 && has_new && !local_jpeg.data.empty()) {
                if (c.last_seq != last_sent_seq) {
                    if (!send_mjpeg_frame(c.fd, local_jpeg)) {
                        dead = true;
                    } else {
                        c.last_seq = last_sent_seq;
                        sent_count++;
                    }
                }
            }

            // 断开
            if (dead) {
                std::cout << "[HTTP] 断开 fd=" << c.fd << std::endl;
                close(c.fd);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---- 3. 清理 ----
    for (auto& c : clients) close(c.fd);
    close(lfd);
    std::cout << "[HTTP] 退出 (发送 " << sent_count << " 帧)" << std::endl;
    return nullptr;
}
