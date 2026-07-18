# ═══════════════════════════════════════════════════════════════════
# opencv.cmake — OpenCV 查找、裁剪、add_subdirectory、include
# ═══════════════════════════════════════════════════════════════════
# 注意：此文件仅在 NOT RK_SKIP_OPENCV 时被包含

# Define OpenCV Root Paths
if(NOT DEFINED OPENCV_ROOT)
    if(DEFINED ENV{OPENCV_ROOT})
        set(OPENCV_ROOT $ENV{OPENCV_ROOT})
    endif()
endif()

if(NOT DEFINED OPENCV_CONTRIB_ROOT)
    if(DEFINED ENV{OPENCV_CONTRIB_ROOT})
        set(OPENCV_CONTRIB_ROOT $ENV{OPENCV_CONTRIB_ROOT})
    endif()
endif()

# Portability: forbid hard-coded absolute dependency paths; require env/cache variables.
if(NOT OPENCV_ROOT)
    message(FATAL_ERROR "OPENCV_ROOT not set. Please use -DOPENCV_ROOT=... or set the OPENCV_ROOT environment variable to the OpenCV source root directory.")
endif()

if(NOT EXISTS "${OPENCV_ROOT}")
    message(FATAL_ERROR "Invalid OPENCV_ROOT path: ${OPENCV_ROOT} does not exist. Please modify the environment variable, or explicitly skip OpenCV with -DRK_SKIP_OPENCV=ON.")
endif()

if(NOT EXISTS "${OPENCV_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "Invalid OPENCV_ROOT path: ${OPENCV_ROOT} is not a valid OpenCV source code directory (missing CMakeLists.txt). It must not be a system installation path. Please modify the environment variable, or explicitly skip OpenCV with -DRK_SKIP_OPENCV=ON.")
endif()

# Configure OpenCV Build
if(OPENCV_CONTRIB_ROOT)
    set(OPENCV_EXTRA_MODULES_PATH "${OPENCV_CONTRIB_ROOT}/modules" CACHE PATH "Path to opencv_contrib modules")
endif()
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
# Whitelist only the modules we actually link. See RK_OPENCV_FULL_LIBS below.
# If BUILD_LIST is already set (e.g. from CMakePresets or -D flag), respect it.
set(BUILD_opencv_world OFF CACHE BOOL "Build opencv_world")
if(NOT BUILD_LIST)
    set(BUILD_LIST "core,imgproc,imgcodecs,objdetect,features2d,flann,calib3d,dnn,ml,photo,video,videoio,highgui,stitching" CACHE STRING "OpenCV modules to build (empty = all)" FORCE)
endif()

# Add OpenCV source subdirectory
# This will build OpenCV as part of the project
add_subdirectory("${OPENCV_ROOT}" "${CMAKE_BINARY_DIR}/opencv")

# Explicitly add include directories for OpenCV modules
# This is necessary because add_subdirectory might not propagate includes automatically for all modules in this context
include_directories(
    "${OPENCV_ROOT}/include"
    "${OPENCV_ROOT}/modules/core/include"
    "${OPENCV_ROOT}/modules/imgproc/include"
    "${OPENCV_ROOT}/modules/video/include"
    "${OPENCV_ROOT}/modules/objdetect/include"
    "${OPENCV_ROOT}/modules/imgcodecs/include"
    "${OPENCV_ROOT}/modules/calib3d/include"
    "${OPENCV_ROOT}/modules/features2d/include"
    "${OPENCV_ROOT}/modules/flann/include"
    "${OPENCV_ROOT}/modules/dnn/include"
    "${OPENCV_ROOT}/modules/ml/include"
    "${OPENCV_ROOT}/modules/photo/include"
    "${OPENCV_ROOT}/modules/videoio/include"
    "${OPENCV_ROOT}/modules/highgui/include"
    "${OPENCV_ROOT}/modules/stitching/include"
    "${OPENCV_CONTRIB_ROOT}/modules/face/include"
    "${CMAKE_BINARY_DIR}"
)

# Log library for Android
if(ANDROID)
    find_library(log-lib log)
    find_library(jnigraphics-lib jnigraphics)
endif()

# ── NCNN 后端 ─────────────────────────────────────
set(RK_HAVE_NCNN OFF)
set(RK_NCNN_TARGET "")
if(RK_ENABLE_NCNN)
    if(NOT DEFINED NCNN_DIR AND DEFINED ENV{NCNN_DIR})
        set(NCNN_DIR $ENV{NCNN_DIR})
    endif()

    # Android multi-ABI: use separate NCNN_DIR per ABI when provided via Gradle
    if(ANDROID)
        if(ANDROID_ABI STREQUAL "armeabi-v7a")
            if(DEFINED NCNN_DIR_ARMv7 AND NOT NCNN_DIR_ARMv7 STREQUAL "")
                set(NCNN_DIR "${NCNN_DIR_ARMv7}")
            endif()
        elseif(ANDROID_ABI STREQUAL "arm64-v8a")
            if(DEFINED NCNN_DIR_ARM64 AND NOT NCNN_DIR_ARM64 STREQUAL "")
                set(NCNN_DIR "${NCNN_DIR_ARM64}")
            endif()
        endif()
    endif()

    find_package(ncnn CONFIG QUIET)

    if(NOT ncnn_FOUND AND RK_NCNN_FETCHCONTENT)
        include(FetchContent)
        set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
        set(NCNN_VULKAN OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            ncnn
            GIT_REPOSITORY ${RK_NCNN_GIT_REPOSITORY}
            GIT_TAG ${RK_NCNN_GIT_TAG}
        )
        FetchContent_MakeAvailable(ncnn)
    endif()

    if(TARGET ncnn::ncnn)
        set(RK_HAVE_NCNN ON)
        set(RK_NCNN_TARGET ncnn::ncnn)
    elseif(TARGET ncnn)
        set(RK_HAVE_NCNN ON)
        set(RK_NCNN_TARGET ncnn)
    endif()
endif()

# ── libyuv 后端 ────────────────────────────────────
set(RK_HAVE_LIBYUV OFF)
set(RK_LIBYUV_TARGET "")
if(RK_ENABLE_LIBYUV)
    include(FetchContent)
    FetchContent_Declare(
        libyuv
        GIT_REPOSITORY https://chromium.googlesource.com/libyuv/libyuv
        GIT_TAG        d9681c53b3af633ab3c64655fcb9625e364b8f9c
        GIT_SUBMODULES ""
    )
    FetchContent_GetProperties(libyuv)
    if(NOT libyuv_POPULATED)
        FetchContent_Populate(libyuv)
    endif()

    if(libyuv_SOURCE_DIR)
        file(GLOB libyuv_sources CONFIGURE_DEPENDS "${libyuv_SOURCE_DIR}/source/*.cc")
        add_library(rk_libyuv STATIC ${libyuv_sources})
        target_include_directories(rk_libyuv PUBLIC ${libyuv_SOURCE_DIR}/include)
        if(MSVC)
            target_compile_options(rk_libyuv PRIVATE /utf-8)
        endif()

        set(RK_HAVE_LIBYUV ON)
        set(RK_LIBYUV_TARGET rk_libyuv)
    endif()
endif()

# ── OpenCV 模块分组变量 ──────────────────────────
set(RK_OPENCV_FULL_LIBS
  opencv_core opencv_imgproc opencv_video opencv_objdetect
  opencv_face opencv_imgcodecs opencv_calib3d opencv_features2d
  opencv_flann opencv_dnn opencv_ml opencv_photo opencv_videoio
  opencv_highgui opencv_stitching
)
