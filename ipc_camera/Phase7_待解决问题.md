# Phase 7 RKNN — 待解决问题 & 交接说明

> **2026-08-15**，Phase 7.1 / 7.2 / **7.3 全部已通**（实时检测接入 RTSP + FreeType 标签）。
> 本文件 4 个「未解决问题」现已**全部解决**。⚠️ 注意：下方「候选解法 #1/#2」当年按错误的 API 猜的，**都不成立**（本 SDK librga 1.10 没有 handle API，也没有 `c_RkRgaBlit`）。
> 实际实现见 `phase7_ai_rtsp/`，完整交接见 `RK3568 开发进度记录.md`（第十五节）。

---

## 一、未解决问题（7.3 要处理的）

### 1. RGA 零拷贝没通 —— 临时用 CPU 转换替代 ⚠️ 最重要

**现状**：`phase7_rknn/src/main.cpp` 用 CPU 转 NV12→RGB（`nv12_to_rgb_cpu`），能出结果。
但 RGA 硬件转换没通。7.3 接实时流时**必须换回 RGA**，否则 CPU 转 RGB 会吃满 CPU，30fps 实时流卡死。

**根因**：librga 2.x 的 handle 混用限制。RGA 日志报错：

```
librga only supports the use of handles only or no handles, [src,dst] = [127, 0]
Failed to call RockChipRga interface
```

即：NV12 源 buffer 被 `wrapbuffer_virtualaddr` 自动转成 handle(127)，而 RGB 目标没有转(handle=0)，
librga 2.x 要求所有 buffer 统一用 handle 模式，混用直接拒。

**已尝试 / 失败记录**：
- `wrapbuffer_virtualaddr` + `imcvtcolor` → 报 handle 混用（上面那个错）
- 老版 `c_RkRgaBlit`（rga.h）→ 没试到底（当时被 im2d.h 路径问题岔开了）

**候选解法（7.3 验证，按概率排）**：
1. 用 librga 2.x 的 handle API：`importbuffer_virtualaddr()` 或 `wrapbuffer_handle_virtualaddr()`
   对 src/dst 都用，返回 `rga_buffer_handle_t`，再调 `imcvtcolor`。
2. 用老版 `c_RkRgaBlit`（rga.h 里，`rga_info_t` + `fd=-1` + `virAddr` + `mmuFlag=1`），绕过 handle 机制。
3. 查官方 demo `external/rknpu2/examples/rknn_yolov5_demo/` 有没有 NV12→RGB 的写法
   （它的 main.cc 只做了 RGB→RGB 的 resize，没做色彩转换，可能帮不上）。

> 参考头文件位置：sysroot `usr/include/rga/` 下有 `im2d.h`、`rga.h`、`RgaUtils.h`、`im2d.hpp`。
> 可以直接 `cat` 这些头文件看真实函数签名（RGA API 版本 1.10.0）。

### 2. 检测框坐标还没映射回摄像头分辨率

检测框坐标在 **640×640 模型空间**，摄像头是 640×480，letterbox 时上下各 pad 80px。
所以摄像头坐标 = 模型坐标 - pad_y(80)。当前 demo 没做这个映射，打印的是模型空间坐标。
7.3 画框到视频时要做：`cam_y = model_y - 80`（x 不变，scale=1.0）。

### 3. 置信度阈值偏低

实测 conf 只有 0.15~0.71。当前 obj/class 阈值 0.25、NMS 0.45 是官方默认。
如果实际场景误检多，可：
- 提高 obj/class 阈值到 0.4~0.5
- 或只在 conf 高时才上报/告警

### 4. 单帧 demo → 实时流（7.3 主任务）

当前 main.cpp 是「抓一帧 → 检测 → 退出」。7.3 要接进 Phase 6 的 RTSP 流水线：
采集线程产出 NV12 → 分两路（MPP 编码 + AI 检测并行），AI 结果叠加到预览流或作元数据。
架构上 AI 是旁路，不阻塞主码流（编码/推流照旧）。

---

## 二、已解决但必记的坑（避免重踩）

| # | 坑 | 真相 | 正确做法 |
|---|----|------|---------|
| 1 | `rknn_init` 报 "Invalid RKNN format" | .rknn 文件被截断(3.3MB) | 完整大小 **8073664**，用 scp 传，`ls -l` 核对 |
| 2 | `im2d.h` 找不到 | 在 sysroot `usr/include/rga/`，不在 `external/linux-rga/include` | `#include <rga/im2d.h>`（配 --sysroot） |
| 3 | `RK_FORMAT_*` 未声明 | librga 2.x 格式枚举在 `rga.h`，不在 `im2d.h` | 加 `#include <rga/rga.h>` |
| 4 | imcvtcolor 返回 0 被判失败 | 此版本 `IM_STATUS_SUCCESS=1`，0 是失败 | 判断用 `IM_STATUS_SUCCESS`，别假设 0=成功 |

---

## 三、7.3 建议步骤

1. 解决 RGA 零拷贝（见问题 1，优先）
2. 把检测框坐标映射回 640×480（见问题 2）
3. 把检测接进 Phase 6 流水线：采集 → 编码 + AI 并行，AI 旁路不阻塞主码流
4. （可选）用 RGA 硬件画框叠加到预览流

---

## 四、当前代码状态速查

- **最终实现 `phase7_ai_rtsp/`**（7.3 实时检测）：四线程（采集/编码/RTSP/AI），RGA 零拷贝 + FreeType 标签，见 `RK3568 开发进度记录.md` 第十五节。
- `phase7_rknn/src/main.cpp`（7.1/7.2 单帧 demo，已归档）：
  - `capture_one_nv12()` — V4L2 抓一帧（复用 Phase 1 逻辑，含丢 9 帧）
  - `nv12_to_rgb_cpu()` — CPU NV12→RGB（BT.601 系数）
  - `decode_one_head()` + `nms()` — YOLOv5 3 头解码 + NMS（后处理已被 7.3 的 `ai_detector.cpp` 复用）
- 模型：板子 `/yolov5s.rknn`（8073664 字节，SDK 里 yolov5s-640-640.rknn 拷贝来的）
