cmake_minimum_required(VERSION 3.10)
project(ipc_ai LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# RKNN 路径 (默认按 SDK 目录布局, 可用 -D 覆盖)
set(SDK_ROOT "$ENV{HOME}/RK3568/SDK/linux/rk3568_linux_sdk" CACHE PATH "RK3568 SDK root")
set(RKNN_INC "${SDK_ROOT}/external/rknpu2/runtime/RK356X/Linux/librknn_api/include")
set(RKNN_LIB "${SDK_ROOT}/external/rknpu2/runtime/RK356X/Linux/librknn_api/aarch64")

add_executable(ipc_ai
    src/main.cpp
    src/v4l2_capture.cpp
    src/h264_encoder.cpp
    src/rtsp_server.cpp
    src/ai_detector.cpp
    src/text_renderer.cpp
)
target_include_directories(ipc_ai PRIVATE src ${RKNN_INC}
    ${CMAKE_SYSROOT}/usr/include/freetype2)
target_link_directories(ipc_ai PRIVATE ${RKNN_LIB})

# RGA: 若 sysroot 无 librga 则注释掉 -lrga 并去掉 HAS_RGA (回退 CPU)
target_compile_definitions(ipc_ai PRIVATE HAS_RGA)
target_link_libraries(ipc_ai PRIVATE rknnrt rknn_api rga rockchip_mpp freetype pthread)
