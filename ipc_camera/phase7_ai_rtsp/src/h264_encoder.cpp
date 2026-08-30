/**
 * h264_encoder.cpp — MPP H.264 硬件编码实现
 *
 * 关键设计 (对比 Phase 4 的失败):
 *   - 条件变量等待新帧 (零 CPU 消耗)
 *   - feed 后立即 harvest (不同步等待, 最多 100 次微轮询)
 *   - 首帧 IDR 时提取 SPS/PPS
 *   - 所有耗时操作在锁外完成
 *
 * Phase 7.3 改动: 编码前把 AI 检测框叠加到 NV12 的 Y 平面 (棋盘格描边),
 *   标签文字用 FreeType 渲染 (白字黑底, 抗锯齿)。AI 是旁路, 检测框可能有
 *   1~2 帧滞后 (AI 推理耗时), 属正常现象。
 */

#include "h264_encoder.h"
#include "text_renderer.h"

extern "C" {
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_venc_cfg.h>
}

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/time.h>

static int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

// ---- NAL 类型检测 ----
static uint8_t nal_type(const uint8_t* d, size_t len) {
    if (len >= 5 && d[0]==0 && d[1]==0 && d[2]==0 && d[3]==1) return d[4] & 0x1F;
    if (len >= 4 && d[0]==0 && d[1]==0 && d[2]==1)          return d[3] & 0x1F;
    return 0;
}

static size_t start_code_len(const uint8_t* d, size_t max_len) {
    if (max_len >= 4 && d[0]==0 && d[1]==0 && d[2]==0 && d[3]==1) return 4;
    if (max_len >= 3 && d[0]==0 && d[1]==0 && d[2]==1)          return 3;
    return 0;
}

// 扫描整个包中是否含有 IDR NAL (type 5)
static bool has_idr(const uint8_t* d, size_t len) {
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

// 从 MPP 输出包中提取 SPS/PPS
static void extract_sps_pps(const uint8_t* pd, size_t pl,
                            std::vector<uint8_t>& sps,
                            std::vector<uint8_t>& pps) {
    size_t pos = 0;
    while (pos < pl) {
        size_t sc = start_code_len(pd + pos, pl - pos);
        if (!sc) { pos++; continue; }
        if (pos + sc >= pl) break;

        uint8_t nt = pd[pos + sc] & 0x1F;

        // 找下一个 start code
        size_t next = pos + sc;
        bool found = false;
        for (size_t j = next; j < pl;) {
            size_t sc2 = start_code_len(pd + j, pl - j);
            if (sc2) { next = j; found = true; break; }
            j++;
        }
        if (!found) next = pl;

        size_t nal_size = next - (pos + sc);
        if (nt == 7 && sps.empty()) {
            sps.assign(pd + pos + sc, pd + pos + sc + nal_size);
        } else if (nt == 8 && pps.empty()) {
            pps.assign(pd + pos + sc, pd + pos + sc + nal_size);
        }
        pos = next;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// 画框: 在 NV12 的 Y 平面上画黑白棋盘格描边 (亮/暗背景都醒目)
//       + 框左上角外侧的「类别 置信度」标签 (FreeType 白字黑底)
// ═══════════════════════════════════════════════════════════════════════
static void draw_boxes_nv12(uint8_t* nv12, int W, int H,
                            const std::vector<AiBox>& boxes) {
    uint8_t* y = nv12;   // Y 平面在最前, UV 平面在后 (只碰 Y)
    const int T = 3;     // 描边粗细 (px)

    auto cx = [&](float v) { int x = (int)(v + 0.5f); return x < 0 ? 0 : (x >= W ? W - 1 : x); };
    auto cy = [&](float v) { int yy = (int)(v + 0.5f); return yy < 0 ? 0 : (yy >= H ? H - 1 : yy); };

    for (auto& b : boxes) {
        int x1 = cx(b.x1), y1 = cy(b.y1);
        int x2 = cx(b.x2), y2 = cy(b.y2);
        if (x2 <= x1 || y2 <= y1) continue;

        // 上/下两条水平边
        for (int t = 0; t < T; t++) {
            int yy_top = y1 + t, yy_bot = y2 - t;
            if (yy_top >= 0 && yy_top < H)
                for (int x = x1; x <= x2; x++)
                    y[yy_top * W + x] = ((x / 3 + yy_top / 3) & 1) ? 255 : 0;
            if (yy_bot >= 0 && yy_bot < H)
                for (int x = x1; x <= x2; x++)
                    y[yy_bot * W + x] = ((x / 3 + yy_bot / 3) & 1) ? 255 : 0;
        }
        // 左/右两条垂直边
        for (int t = 0; t < T; t++) {
            int xx_l = x1 + t, xx_r = x2 - t;
            if (xx_l >= 0 && xx_l < W)
                for (int yy = y1; yy <= y2; yy++)
                    y[yy * W + xx_l] = ((xx_l / 3 + yy / 3) & 1) ? 255 : 0;
            if (xx_r >= 0 && xx_r < W)
                for (int yy = y1; yy <= y2; yy++)
                    y[yy * W + xx_r] = ((xx_r / 3 + yy / 3) & 1) ? 255 : 0;
        }

        // 标签: 框左上角外侧 (顶部放不下则内侧), 白字黑底
        int lh = text_line_height();
        int ly = y1 - lh - 1;
        if (ly < 0) ly = y1 + 1;
        draw_text_y(y, W, H, x1, ly, b.label);
    }
}

// ═══════════════════════════════════════════════════════════════════════
void* encoder_thread(void* arg) {
    SharedState* s = static_cast<SharedState*>(arg);
    int W = s->width, H = s->height;
    size_t fsize = (size_t)W * H * 3 / 2;

    // ---- 0. 文字渲染器 (标签用, 失败则只画框不画字) ----
    text_renderer_init(s->ai_font_path.c_str(), s->ai_font_height);

    // ---- 1. 创建 MPP 编码器 ----
    MppCtx  ctx  = nullptr;
    MppApi* mpi  = nullptr;
    if (mpp_create(&ctx, &mpi) != MPP_OK) {
        std::cerr << "[编码] ❌ mpp_create 失败" << std::endl;
        text_renderer_deinit();
        return nullptr;
    }
    if (mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        std::cerr << "[编码] ❌ mpp_init 失败" << std::endl;
        mpp_destroy(ctx);
        text_renderer_deinit();
        return nullptr;
    }

    // ---- 2. 配置编码参数 ----
    MppEncCfg cfg = nullptr;
    mpp_enc_cfg_init(&cfg);
    mpp_enc_cfg_set_s32(cfg, "prep:width",      W);
    mpp_enc_cfg_set_s32(cfg, "prep:height",     H);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", W);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", H);
    mpp_enc_cfg_set_s32(cfg, "prep:format",     MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "rc:mode",         MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps",          2000000);
    mpp_enc_cfg_set_s32(cfg, "h264:gop",        30);  // 每30帧一个IDR
    mpi->control(ctx, MPP_ENC_SET_CFG, cfg);

    // ---- 3. Buffer Group (供 MPP 内部分配) ----
    MppBufferGroup grp = nullptr;
    mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM);
    mpp_buffer_group_limit_config(grp, fsize, 16);

    std::cout << "[编码] MPP H.264 CBR 2Mbps GOP=30 就绪" << std::endl;

    // ---- 4. 编码主循环 ----
    uint64_t last_seq = 0;
    int frame_cnt = 0, last_cnt = 0;
    int64_t last_log = now_us();

    while (s->running) {
        // ---- 4a. 等待新 NV12 帧 (条件变量, 零 CPU) ----
        pthread_mutex_lock(&s->nv12_mutex);
        while (s->nv12_seq == last_seq && s->running) {
            pthread_cond_wait(&s->nv12_cond, &s->nv12_mutex);
        }
        if (!s->running) {
            pthread_mutex_unlock(&s->nv12_mutex);
            break;
        }

        // 拷贝 NV12 (O(1) swap)
        Nv12Frame local;
        local.data.swap(s->nv12.data);
        local.pts_us = s->nv12.pts_us;
        last_seq = s->nv12_seq;
        pthread_mutex_unlock(&s->nv12_mutex);

        // ---- 4b. 画 AI 检测框 + 标签 (读最新结果, 锁外绘制) ----
        if (s->ai_draw_boxes) {
            std::vector<AiBox> boxes;
            pthread_mutex_lock(&s->ai_result.mutex);
            boxes = s->ai_result.boxes;
            pthread_mutex_unlock(&s->ai_result.mutex);
            if (!boxes.empty())
                draw_boxes_nv12(local.data.data(), W, H, boxes);
        }

        // ---- 4c. 投喂 MPP (在锁外) ----
        MppBuffer mb = nullptr;
        mpp_buffer_get(grp, &mb, fsize);
        memcpy((uint8_t*)mpp_buffer_get_ptr(mb), local.data.data(),
               std::min(local.data.size(), fsize));

        MppFrame mf = nullptr;
        mpp_frame_init(&mf);
        mpp_frame_set_width(mf, W);
        mpp_frame_set_height(mf, H);
        mpp_frame_set_hor_stride(mf, W);
        mpp_frame_set_ver_stride(mf, H);
        mpp_frame_set_fmt(mf, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(mf, mb);
        mpp_buffer_put(mb);  // frame 持有引用
        mpi->encode_put_frame(ctx, mf);
        mpp_frame_deinit(&mf);

        // ---- 4d. 收割输出 (微轮询, 不用 sleep) ----
        for (int i = 0; i < 100; i++) {
            MppPacket pkt = nullptr;
            MPP_RET ret = mpi->encode_get_packet(ctx, &pkt);

            if (ret == MPP_OK && pkt) {
                void*  pd = mpp_packet_get_data(pkt);
                size_t pl = mpp_packet_get_length(pkt);

                if (pd && pl > 0) {
                    H264Packet hp;
                    hp.data.assign((uint8_t*)pd, (uint8_t*)pd + pl);
                    hp.pts_us = local.pts_us;
                    hp.is_idr = has_idr((uint8_t*)pd, pl);

                    // 提取 SPS/PPS (首帧 IDR 时)
                    if (!s->sps_pps_ready && hp.is_idr) {
                        pthread_mutex_lock(&s->sps_mutex);
                        if (!s->sps_pps_ready) {
                            extract_sps_pps((uint8_t*)pd, pl, s->sps, s->pps);
                            if (!s->sps.empty() && !s->pps.empty()) {
                                s->sps_pps_ready = true;
                                std::cout << "[编码] SPS/PPS 已提取 (SPS="
                                          << s->sps.size() << "B PPS="
                                          << s->pps.size() << "B)" << std::endl;
                            }
                        }
                        pthread_mutex_unlock(&s->sps_mutex);
                    }

                    // 推入环形缓冲
                    s->h264_ring->push(std::move(hp));
                }
                mpp_packet_deinit(&pkt);
            } else {
                // 没有更多输出, 退出收割循环
                break;
            }
        }

        // ---- 4e. 日志 ----
        frame_cnt++;
        if ((frame_cnt % 30) == 0) {
            int64_t now = now_us();
            double fps = (double)(frame_cnt - last_cnt) * 1e6 /
                         (double)(now - last_log);
            std::cout << "[编码] " << frame_cnt << " 帧 ("
                      << (int)(fps + 0.5) << "fps) ring="
                      << s->h264_ring->size() << "/" << SharedState::RING_SIZE
                      << std::endl;
            last_log = now;
            last_cnt = frame_cnt;
        }
    }

    // ---- 5. Flush (发送空帧触发编码器输出剩余帧) ----
    mpi->encode_put_frame(ctx, nullptr);
    for (int i = 0; i < 50; i++) {
        MppPacket pkt = nullptr;
        if (mpi->encode_get_packet(ctx, &pkt) == MPP_OK && pkt) {
            void* pd = mpp_packet_get_data(pkt);
            size_t pl = mpp_packet_get_length(pkt);
            if (pd && pl > 0) {
                H264Packet hp;
                hp.data.assign((uint8_t*)pd, (uint8_t*)pd + pl);
                hp.pts_us = -1;
                s->h264_ring->push(std::move(hp));
            }
            mpp_packet_deinit(&pkt);
        } else {
            usleep(2000);
        }
    }

    // ---- 6. 清理 ----
    mpp_buffer_group_put(grp);
    mpp_enc_cfg_deinit(cfg);
    mpi->reset(ctx);
    mpp_destroy(ctx);
    text_renderer_deinit();

    s->h264_ring->wake_all();
    std::cout << "[编码] 退出 (" << frame_cnt << " 帧)" << std::endl;
    return nullptr;
}
