# Cross-compilation toolchain for RK3568 (Buildroot)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-rockchip-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-rockchip-linux-gnu-g++)

set(CMAKE_SYSROOT "$ENV{HOME}/RK3568/SDK/linux/rk3568_linux_sdk/buildroot/output/rockchip_rk3568/host/aarch64-buildroot-linux-gnu/sysroot")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
