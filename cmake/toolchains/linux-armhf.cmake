# Linux armhf（ARMv7 hard-float）交叉工具链，对应 RK3288 的 Cortex-A17 / NEON。
# CPU/FPU 选项与 CMakeLists.txt 中 Android armeabi-v7a 的目标一致；Linux armhf 惯用硬浮点 ABI。
# 用途：CI `linux-arm-cross` 的 armhf 列，以及本地 `cmake --toolchain` 交叉编译验证。只编不跑。
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
