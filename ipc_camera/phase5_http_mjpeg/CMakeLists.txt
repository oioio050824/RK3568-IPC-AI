cmake_minimum_required(VERSION 3.10)
project(ipc_mjpeg LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sources
add_executable(ipc_mjpeg
    src/main.cpp
    src/v4l2_capture.cpp
    src/jpeg_encoder.cpp
    src/http_server.cpp
)
target_include_directories(ipc_mjpeg PRIVATE src)
target_link_libraries(ipc_mjpeg PRIVATE pthread)

# Optional: libjpeg / libjpeg-turbo for faster encoding
find_library(JPEG_LIB NAMES jpeg turbojpeg)
if(JPEG_LIB)
    target_link_libraries(ipc_mjpeg PRIVATE ${JPEG_LIB})
    target_compile_definitions(ipc_mjpeg PRIVATE HAS_LIBJPEG)
    message(STATUS "Using libjpeg for JPEG encoding")
else()
    message(STATUS "Using bundled tiny JPEG encoder (no libjpeg found)")
endif()
