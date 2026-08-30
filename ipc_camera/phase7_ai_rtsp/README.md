# Phase 7.3 — H.264 + RTSP + RKNN 实时目标检测

把 Phase 7.2 的「单帧 RKNN 检测」接入 Phase 6 的「实时 RTSP 流水线」，AI 作为**旁路**运行，检测框叠加到 H.264 视频流上。

## 架构（四线程，AI 旁路不阻塞主码流）

```
采集(V4L2) ──NV12──▶ 编码(MPP H.264) ──H264环──▶ RTSP   ← 主码流, 与 Phase 6 完全一致
    └─NV12单槽──▶ AI(RKNN检测) ──框──▶ 编码(画框)          ← 旁路, 慢则自动丢帧
```

- 采集线程每帧产出**两份** NV12 副本，分别喂给编码槽和 AI 槽（独立锁，互不阻塞）。
- AI 线程「最新帧覆盖」：RKNN 推理慢（~30-50ms）时自动丢中间帧，永不反压采集。
- 检测框坐标从模型空间 640×640 映射回摄像头 640×480（`cam_y = model_y - 80`），再画到 NV12 的 Y 平面（黑白棋盘格描边，亮暗背景都醒目）。

## 关键改动 vs Phase 6

| 文件 | 改动 |
|------|------|
| `frame_types.h` | 新增 `AiBox`/`AiResult`、AI 单槽、AI 配置 |
| `v4l2_capture.cpp` | 每帧多产出一份 AI 副本 |
| `h264_encoder.cpp` | 编码前画框（`draw_boxes_nv12`） |
| `ai_detector.cpp` | 🆕 RKNN + RGA + 解码 NMS + 坐标映射 |
| `main.cpp` | 新增 AI 线程 + 优雅退出 |
| 其余 | 从 Phase 6 原样复制 |

## RGA 零拷贝（已解决，待板子验证）

> ⚠️ 实测发现本 SDK 的 librga 1.10 **没有** `importbuffer_virtualaddr` / `wrapbuffer_handle` /
> `rga_buffer_handle_t` 这套 handle API，也没有旧版 `c_RkRgaBlit`。文档里两个候选解法都不成立。
> 真实 API 是 `wrapbuffer_virtualaddr` + `imcvtcolor`（都是宏）。

正确写法（都走「无 handle」模式，天然避开 `[127,0]` 句柄混用）：

```c
rga_buffer_t src = wrapbuffer_virtualaddr(nv12, w, h, RK_FORMAT_YCbCr_420_SP);
rga_buffer_t dst = wrapbuffer_virtualaddr(rgb,  w, h, RK_FORMAT_RGB_888);
IM_STATUS st = imcvtcolor(src, dst, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_RGB_888);
// st == IM_STATUS_SUCCESS(=1) 才算成功, IM_STATUS_FAILED(=0)
```

注：`wrapbuffer_virtualaddr` 是宏，签名 `(vir_addr, width, height, format[, wstride, hstride])`。

**兜底**：RGA 一旦失败自动锁死回退 CPU 转换（`nv12_to_rgb_cpu`），并打印一次性警告，demo 永不因 RGA 挂掉。

## 编译运行

```bash
cd phase7_ai_rtsp
./deploy.sh
```

deploy.sh 会自动检测 sysroot 有无 `librga.so`：
- 有 → 加 `-DHAS_RGA -lrga`（硬件转换）
- 无 → 纯 CPU 转换（可跑但 30fps 吃 CPU）

播放：

```bash
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay -framedrop rtsp://192.168.0.200:8554/stream
```

## 可调参数（命令行）

```bash
./ipc_ai 640 480 8554 0.25 /yolov5s.rknn
#         W    H   port  conf阈值  模型路径
```

- `conf阈值`：实测 conf 0.15~0.71，误检多可调到 0.4~0.5（`Phase7_待解决问题.md` 问题 3）。
- 画框开关：`frame_types.h` 里 `ai_draw_boxes` 改 `false` 可关闭叠加（纯元数据模式）。

## 待板子验证项

1. RGA `importbuffer_virtualaddr` 对 V4L2 mmap buffer 和堆内存是否都导入成功（若失败会打印警告并回退 CPU，仍能出结果）。
2. 30fps 下 NPU + VPU + RGA 三者并行的 CPU 占用（预期主码流不受 AI 拖累）。
3. RGB888 色序（若检测框定位准但颜色异常，是 RGA 色序问题，不影响检测）。
