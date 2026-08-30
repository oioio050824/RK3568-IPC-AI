#pragma once
/**
 * rtsp_server.h — RTSP/RTP 流媒体服务器
 *
 * 协议: RTSP (RFC 2326) + RTP over TCP interleaved (RFC 2326 §10.12)
 *       + H.264 over RTP (RFC 6184)
 *
 * I/O 模型: select() 事件循环, 非阻塞发送
 * 数据源: 从 SharedState.h264_ring 消费 H.264 帧
 */

#include "frame_types.h"

void* rtsp_thread(void* state);
