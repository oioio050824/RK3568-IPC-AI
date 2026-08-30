#pragma once
/**
 * http_server.h — HTTP MJPEG 流媒体服务器
 *
 * 协议: HTTP/1.0 + multipart/x-mixed-replace
 * I/O 模型: select() 事件循环, 非阻塞发送
 *
 * 端点:
 *   GET /          — MJPEG 实时视频流
 *   GET /snapshot  — 单帧 JPEG (调试用)
 *
 * 特性:
 *   - 多客户端支持 (每客户端独立 fd)
 *   - 慢客户端保护 (send buffer 满时跳过该 fd)
 *   - select() 可中断退出
 */

#include "frame_types.h"

/**
 * HTTP 服务器线程入口。
 * @param arg  SharedState 指针
 * @return     nullptr
 */
void* http_server_thread(void* arg);
