/**
 * jpeg_encoder.cpp — JPEG 编码实现
 *
 * NV12→RGB 转换 (ITU-R BT.601 定点整数) + JPEG 压缩。
 *
 * 后端优先级:
 *   1. libjpeg-turbo (CMake 检测到 HAS_LIBJPEG)
 *   2. 内置 tiny JPEG 编码器 (零依赖)
 */

#include "jpeg_encoder.h"

#ifdef HAS_LIBJPEG
#include <cstdio>       // FILE*, fmemopen (或手动拼接)
#include <jpeglib.h>
#endif

#include "tiny_jpeg_encoder.h"

#include <iostream>
#include <cstring>
#include <sys/time.h>

// ═══════════════════════════════════════════════════════════════════════
// NV12 → RGB 转换 (ITU-R BT.601, 定点整数)
// ═══════════════════════════════════════════════════════════════════════

static void nv12_to_rgb(const uint8_t* nv12, uint8_t* rgb,
                         int W, int H) {
    const uint8_t* Y  = nv12;
    const uint8_t* UV = nv12 + (size_t)W * H;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int yy = (int)Y[y * W + x];
            int uv_ofs = ((y / 2) * W + (x & ~1));
            int uu = (int)UV[uv_ofs]     - 128;
            int vv = (int)UV[uv_ofs + 1] - 128;

            // ITU-R BT.601 full swing, 8-bit fixed point
            int r = yy               + ((359 * vv) >> 8);
            int g = yy - ((88 * uu)  >> 8) - ((183 * vv) >> 8);
            int b = yy + ((454 * uu) >> 8);

            rgb[0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            rgb[1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            rgb[2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
            rgb += 3;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// 辅助: 微秒时间戳
// ═══════════════════════════════════════════════════════════════════════

static int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

// ═══════════════════════════════════════════════════════════════════════
// TIER 1: libjpeg-turbo JPEG 编码
// ═══════════════════════════════════════════════════════════════════════

#ifdef HAS_LIBJPEG

static std::vector<uint8_t> encode_jpeg_libjpeg(const uint8_t* rgb,
                                                  int W, int H, int quality) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    // 输出到内存
    unsigned char* outbuf = nullptr;
    unsigned long  outlen = 0;
    jpeg_mem_dest(&cinfo, &outbuf, &outlen);

    cinfo.image_width      = (JDIMENSION)W;
    cinfo.image_height     = (JDIMENSION)H;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    // 逐行写入
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + cinfo.next_scanline * W * 3);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);

    std::vector<uint8_t> result(outbuf, outbuf + outlen);
    free(outbuf);  // jpeg_mem_dest 用 malloc 分配, 需要 free
    jpeg_destroy_compress(&cinfo);

    return result;
}

#endif // HAS_LIBJPEG

// ═══════════════════════════════════════════════════════════════════════
// 编码线程主循环
// ═══════════════════════════════════════════════════════════════════════

void* encoder_thread(void* arg) {
    SharedState* s = static_cast<SharedState*>(arg);
    int W = s->width, H = s->height;
    size_t fsize = (size_t)W * H * 3 / 2;

    // 预分配 RGB 缓冲区 (重用, 避免每次分配)
    std::vector<uint8_t> rgb_buf((size_t)W * H * 3);
    uint64_t last_seq = 0;
    uint64_t enc_seq  = 0;

    int64_t last_log = now_us();
    int    frame_cnt = 0, last_cnt = 0;

#ifdef HAS_LIBJPEG
    std::cout << "[编码] 使用 libjpeg-turbo 后端" << std::endl;
#else
    std::cout << "[编码] 使用内置 tiny JPEG 编码器" << std::endl;
#endif

    while (s->running) {
        // ---- 1. 等待新帧 (条件变量, 零 CPU 消耗) ----
        pthread_mutex_lock(&s->nv12_mutex);

        // 检查是否已有新帧 (避免错过 signal 时的竞争)
        while (s->nv12_seq == last_seq && s->running) {
            pthread_cond_wait(&s->nv12_cond, &s->nv12_mutex);
        }

        if (!s->running) {
            pthread_mutex_unlock(&s->nv12_mutex);
            break;
        }

        // ---- 2. 拷贝 NV12 数据 (锁内, 但只是一次 vector swap) ----
        Nv12Frame local_nv12;
        local_nv12.data.swap(s->nv12.data);  // O(1) 指针交换
        local_nv12.pts_us = s->nv12.pts_us;
        last_seq = s->nv12_seq;
        pthread_mutex_unlock(&s->nv12_mutex);

        // ---- 3. NV12 → RGB (在锁外, 耗时 ~3-5ms) ----
        if (local_nv12.data.size() >= fsize) {
            nv12_to_rgb(local_nv12.data.data(), rgb_buf.data(), W, H);
        } else {
            continue;  // 不完整帧, 跳过
        }

        // ---- 4. RGB → JPEG (在锁外, 耗时 ~10-20ms) ----
        std::vector<uint8_t> jpeg_data;
#ifdef HAS_LIBJPEG
        jpeg_data = encode_jpeg_libjpeg(rgb_buf.data(), W, H, 75);
#else
        jpeg_data = encode_jpeg(rgb_buf.data(), W, H, 75);
#endif

        if (jpeg_data.empty()) continue;

        // ---- 5. 写入 JPEG slot (锁内, 仅 swap) ----
        pthread_mutex_lock(&s->jpeg_mutex);
        s->jpeg.set_data(jpeg_data.data(), jpeg_data.size(),
                         local_nv12.pts_us, ++enc_seq);
        pthread_mutex_unlock(&s->jpeg_mutex);

        // ---- 6. 日志 ----
        frame_cnt++;
        if ((frame_cnt % 30) == 0) {
            int64_t now = now_us();
            double fps = (double)(frame_cnt - last_cnt) * 1e6 / (double)(now - last_log);
            std::cout << "[编码] " << frame_cnt << " 帧 "
                      << "(" << (int)(fps + 0.5) << "fps) "
                      << "JPEG=" << jpeg_data.size() << "B "
                      << "(" << jpeg_data.size() * 100 / fsize << "%)" << std::endl;
            last_log = now;
            last_cnt = frame_cnt;
        }
    }

    std::cout << "[编码] 退出 (" << frame_cnt << " 帧)" << std::endl;
    return nullptr;
}
