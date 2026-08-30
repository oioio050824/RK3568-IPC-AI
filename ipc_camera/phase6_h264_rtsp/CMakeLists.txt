cmake_minimum_required(VERSION 3.10)
project(ipc_rtsp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ipc_rtsp
    src/main.cpp
    src/v4l2_capture.cpp
    src/h264_encoder.cpp
    src/rtsp_server.cpp
)
target_include_directories(ipc_rtsp PRIVATE src)
target_link_libraries(ipc_rtsp PRIVATE rockchip_mpp pthread)
