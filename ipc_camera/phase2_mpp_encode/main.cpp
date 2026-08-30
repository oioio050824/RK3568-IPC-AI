/**
 * Phase 2: MPP H.264 硬件编码
 *
 * 流程: 读取 NV12 帧文件 → MPP 创建编码器 → 配置参数
 *       → 逐帧送入 → 轮询取 H.264 包 → 写入文件 → Flush → 清理退出
 *
 * 编译: aarch64-rockchip-linux-gnu-g++ -std=c++17 -O2 \
 *       --sysroot=<buildroot_sysroot> \
 *       -o encoder main.cpp -lrockchip_mpp
 *
 * 用法: ./encoder [input.yuv] [output.h264] [width] [height]
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <iomanip>

extern "C" {
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_venc_cfg.h>
}

// ---------------------------------------------------------------------------
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "[错误] " << msg << ": " << std::strerror(errno)      \
                      << std::endl;                                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#define MPP_CHECK(ret, msg)                                                    \
    do {                                                                       \
        if ((ret) != MPP_OK) {                                                 \
            std::cerr << "[错误] " << msg << ": ret=" << (int)(ret)            \
                      << std::endl;                                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
static const char* nal_type_name(uint8_t type) {
    switch (type) {
        case 1:  return "P/B帧";
        case 5:  return "IDR(关键帧)";
        case 6:  return "SEI";
        case 7:  return "SPS";
        case 8:  return "PPS";
        default: return "其他";
    }
}

static uint8_t detect_nal(const uint8_t *data, size_t len) {
    if (len >= 5 && data[0]==0x00 && data[1]==0x00 && data[2]==0x00 && data[3]==0x01)
        return data[4] & 0x1F;
    if (len >= 4 && data[0]==0x00 && data[1]==0x00 && data[2]==0x01)
        return data[3] & 0x1F;
    return 0;
}

// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    const char *in_file   = (argc > 1) ? argv[1] : "input.yuv";
    const char *out_file  = (argc > 2) ? argv[2] : "output.h264";
    int         width     = (argc > 3) ? std::stoi(argv[3]) : 640;
    int         height    = (argc > 4) ? std::stoi(argv[4]) : 480;

    std::cout << "=== RK3568 MPP H.264 硬编码 (Phase 2) ===" << std::endl;
    std::cout << "输入: " << in_file << ", 输出: " << out_file << std::endl;
    std::cout << "分辨率: " << width << "x" << height << std::endl;

    size_t frame_size = width * height * 3 / 2;  // NV12 = W*H*3/2

    // ---- 1. 读取 NV12 文件 ------------------------------------------------
    std::ifstream in(in_file, std::ios::binary | std::ios::ate);
    CHECK(in.is_open(), "无法打开输入文件");
    size_t file_size = in.tellg();
    in.seekg(0);
    CHECK(file_size >= frame_size, "输入文件太小(不足一帧)");

    size_t num_frames = file_size / frame_size;
    std::cout << "文件: " << file_size << " 字节, " << num_frames << " 帧" << std::endl;

    std::vector<uint8_t> nv12(file_size);
    in.read(reinterpret_cast<char*>(nv12.data()), file_size);
    in.close();
    std::cout << "[1] 读取 NV12 文件... OK" << std::endl;

    // ---- 2. 创建 MPP 编码器 -----------------------------------------------
    MppCtx  ctx  = nullptr;
    MppApi  *mpi = nullptr;

    MPP_RET ret = mpp_create(&ctx, &mpi);
    MPP_CHECK(ret, "mpp_create");
    std::cout << "[2] mpp_create... OK" << std::endl;

    ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    MPP_CHECK(ret, "mpp_init");
    std::cout << "[3] mpp_init... OK" << std::endl;

    // ---- 3. 配置编码器 ----------------------------------------------------
    MppEncCfg cfg = nullptr;
    ret = mpp_enc_cfg_init(&cfg);
    MPP_CHECK(ret, "mpp_enc_cfg_init");

    mpp_enc_cfg_set_s32(cfg, "prep:width",  width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);   // NV12
    mpp_enc_cfg_set_s32(cfg, "rc:mode",     MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps",      2000000);             // 2 Mbps
    mpp_enc_cfg_set_s32(cfg, "codec:type",  MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);                // High
    mpp_enc_cfg_set_s32(cfg, "h264:level",   40);                 // 4.0

    ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    MPP_CHECK(ret, "MPP_ENC_SET_CFG");
    std::cout << "[4] 配置编码器... OK (CBR 2Mbps, H.264 High@4.0)" << std::endl;

    // ---- 4. 创建 buffer group (编码器输入需要硬件 DMA 兼容内存) ----------
    MppBufferGroup buf_group = nullptr;
    ret = mpp_buffer_group_get_internal(&buf_group, MPP_BUFFER_TYPE_DRM);
    MPP_CHECK(ret, "mpp_buffer_group_get_internal");

    // 给 4 个 buffer 的空间
    ret = mpp_buffer_group_limit_config(buf_group, frame_size, 4);
    std::cout << "[5] buffer group... OK" << std::endl;

    // ---- 5. 打开输出文件 -----------------------------------------------
    std::ofstream out(out_file, std::ios::binary);
    CHECK(out.is_open(), "无法创建输出文件");

    // ---- 6. 逐帧编码 ---------------------------------------------------
    int nal_cnt[32] = {0};
    size_t total_out = 0;

    for (size_t f = 0; f < num_frames; f++) {
        // 6a. 从 buffer group 申请一个 buffer
        MppBuffer buf = nullptr;
        ret = mpp_buffer_get(buf_group, &buf, frame_size);
        MPP_CHECK(ret, "mpp_buffer_get");

        // 6b. 拷贝 NV12 数据到硬件 buffer
        void *dst = mpp_buffer_get_ptr(buf);
        CHECK(dst != nullptr, "mpp_buffer_get_ptr");
        memcpy(dst, nv12.data() + f * frame_size, frame_size);

        // 6c. 创建 frame, 绑定 buffer
        MppFrame frame = nullptr;
        ret = mpp_frame_init(&frame);
        MPP_CHECK(ret, "mpp_frame_init");

        mpp_frame_set_width(frame, width);
        mpp_frame_set_height(frame, height);
        mpp_frame_set_hor_stride(frame, width);
        mpp_frame_set_ver_stride(frame, height);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(frame, buf);

        // 释放 buffer 引用（frame 持有了一份）
        mpp_buffer_put(buf);
        buf = nullptr;

        // 6d. 送入编码器
        ret = mpi->encode_put_frame(ctx, frame);
        MPP_CHECK(ret, "encode_put_frame");
        mpp_frame_deinit(&frame);

        // 6e. 轮询取输出包
        for (int t = 0; t < 30; t++) {
            MppPacket packet = nullptr;
            ret = mpi->encode_get_packet(ctx, &packet);

            if (ret == MPP_OK && packet) {
                void *d = mpp_packet_get_data(packet);
                size_t len = mpp_packet_get_length(packet);

                if (d && len > 0) {
                    uint8_t nal = detect_nal((const uint8_t*)d, len);
                    if (nal < 32) nal_cnt[nal]++;
                    out.write((const char*)d, len);
                    total_out += len;
                    std::cout << "  [帧" << (f+1) << "] " << len
                              << "B NAL=" << (int)nal
                              << " (" << nal_type_name(nal) << ")" << std::endl;
                }
                mpp_packet_deinit(&packet);
            } else {
                usleep(5000);  // 5ms
                if (ret != MPP_OK) break;
            }
        }
    }

    std::cout << "[6] 编码完成, " << total_out << " 字节" << std::endl;

    // ---- 7. Flush ------------------------------------------------------
    std::cout << "[7] Flush..." << std::endl;
    ret = mpi->encode_put_frame(ctx, nullptr);
    if (ret != MPP_OK)
        std::cerr << "  [警告] flush put_frame: " << (int)ret << std::endl;

    for (int t = 0; t < 30; t++) {
        MppPacket packet = nullptr;
        ret = mpi->encode_get_packet(ctx, &packet);

        if (ret == MPP_OK && packet) {
            void *d = mpp_packet_get_data(packet);
            size_t len = mpp_packet_get_length(packet);

            if (d && len > 0) {
                uint8_t nal = detect_nal((const uint8_t*)d, len);
                if (nal < 32) nal_cnt[nal]++;
                out.write((const char*)d, len);
                total_out += len;
                std::cout << "  [flush] " << len << "B NAL=" << (int)nal << std::endl;
            }
            mpp_packet_deinit(&packet);
        } else {
            usleep(10000);
            if (ret != MPP_OK) break;
        }
    }
    out.close();
    std::cout << "  Flush 完成, 总计 " << total_out << " 字节" << std::endl;

    // ---- 8. 清理 -------------------------------------------------------
    if (buf_group) mpp_buffer_group_put(buf_group);
    if (cfg)       mpp_enc_cfg_deinit(cfg);
    if (ctx) {
        mpi->reset(ctx);
        mpp_destroy(ctx);
    }
    std::cout << "[8] 清理完成" << std::endl;

    // ---- 9. 统计 -------------------------------------------------------
    std::cout << "\n========== 统计 ==========" << std::endl;
    std::cout << "输入: " << num_frames << " 帧 (" << file_size << "B NV12)" << std::endl;
    std::cout << "输出: " << total_out << "B H.264" << std::endl;
    if (total_out > 0)
        std::cout << "压缩比: " << std::fixed << std::setprecision(1)
                  << (double)file_size / total_out << ":1" << std::endl;

    std::cout << "NAL 分布:";
    if (nal_cnt[7]) std::cout << " SPS=" << nal_cnt[7];
    if (nal_cnt[8]) std::cout << " PPS=" << nal_cnt[8];
    if (nal_cnt[5]) std::cout << " IDR=" << nal_cnt[5];
    if (nal_cnt[1]) std::cout << " P=" << nal_cnt[1];
    if (nal_cnt[6]) std::cout << " SEI=" << nal_cnt[6];
    std::cout << std::endl;

    if (total_out > 0 && nal_cnt[7] && nal_cnt[8] && nal_cnt[5])
        std::cout << "\n✅ SPS+PPS+IDR 完整, 编码正常" << std::endl;

    std::cout << "=== Phase 2 完成 ===" << std::endl;
    return 0;
}
