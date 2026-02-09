cmake_minimum_required(VERSION 3.18.1)

project(RK3288_AI_Engine)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags
# -mcpu=cortex-a17 and -mfpu=neon-vfpv4 are specific to the RK3288 (armeabi-v7a)
# We only apply them when targeting that architecture to avoid errors on x86_64 or arm64-v8a.
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mcpu=cortex-a17 -mfpu=neon-vfpv4 -ftree-vectorize")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
endif()

# Include directories
include_directories(app/src/main/cpp/include)

# Configure OpenCV Build
set(OPENCV_EXTRA_MODULES_PATH "D:/ProgramData/OpenCV/opencv_contrib-4.10.0/modules" CACHE PATH "Path to opencv_contrib modules")
# Disable unnecessary modules to speed up build
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries")
set(BUILD_TESTS OFF CACHE BOOL "Build tests")
set(BUILD_PERF_TESTS OFF CACHE BOOL "Build perf tests")
set(BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
set(BUILD_ANDROID_EXAMPLES OFF CACHE BOOL "Build Android examples")
set(BUILD_DOCS OFF CACHE BOOL "Build docs")
set(BUILD_JAVA OFF CACHE BOOL "Build Java wrapper") # We use C++ only
set(BUILD_opencv_python2 OFF CACHE BOOL "Build Python2")
set(BUILD_opencv_python3 OFF CACHE BOOL "Build Python3")
# Explicitly whitelist modules if needed, or rely on default dependencies
# We definitely need: core, imgproc, video, objdetect, face, imgcodecs
set(BUILD_opencv_world OFF CACHE BOOL "Build opencv_world")

# Add OpenCV source subdirectory
# This will build OpenCV as part of the project
add_subdirectory("D:/ProgramData/OpenCV/opencv-4.10.0" "${CMAKE_BINARY_DIR}/opencv")

# Explicitly add include directories for OpenCV modules
# This is necessary because add_subdirectory might not propagate includes automatically for all modules in this context
include_directories(
    "D:/ProgramData/OpenCV/opencv-4.10.0/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/core/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/imgproc/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/video/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/objdetect/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/imgcodecs/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/calib3d/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/features2d/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/flann/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/dnn/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/ml/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/photo/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/videoio/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/highgui/include"
    "D:/ProgramData/OpenCV/opencv-4.10.0/modules/stitching/include"
    "D:/ProgramData/OpenCV/opencv_contrib-4.10.0/modules/face/include"
    "${CMAKE_BINARY_DIR}"
)

# Log library for Android
find_library(log-lib log)
find_library(jnigraphics-lib jnigraphics)

# Collect Core Source Files (excluding entry points)
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "app/src/main/cpp/src/*.cpp"
)

# 1. Android JNI Shared Library
add_library(native-lib SHARED ${CORE_SOURCES} "app/src/main/cpp/native-lib.cpp")

# Link Libraries for JNI
target_link_libraries(
    native-lib
    opencv_core
    opencv_imgproc
    opencv_video
    opencv_objdetect
    opencv_face
    opencv_imgcodecs
    opencv_calib3d
    opencv_features2d
    opencv_flann
    opencv_dnn
    opencv_ml
    opencv_photo
    opencv_videoio
    opencv_highgui
    opencv_stitching
    ${log-lib}
    ${jnigraphics-lib}
    android
)

# Explicitly set include directories for native-lib
target_include_directories(native-lib PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/cpp/include
)

# 2. Native Executable (CLI) for RK3288
add_executable(rk3288_cli "app/src/main/cpp/main.cpp" ${CORE_SOURCES})

# Explicitly set include directories for rk3288_cli
target_include_directories(rk3288_cli PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/cpp/include
)

# Link Libraries for CLI
target_link_libraries(
    rk3288_cli
    opencv_core
    opencv_imgproc
    opencv_video
    opencv_objdetect
    opencv_face
    opencv_imgcodecs
    opencv_calib3d
    opencv_features2d
    opencv_flann
    opencv_dnn
    opencv_ml
    opencv_photo
    opencv_videoio
    opencv_highgui
    opencv_stitching
    ${log-lib}
    ${jnigraphics-lib}
    android
)
