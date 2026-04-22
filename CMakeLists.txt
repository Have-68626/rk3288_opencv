cmake_minimum_required(VERSION 3.18.1)

project(RK3288_AI_Engine)

# Version definition for Windows/CLI
set(APP_BUILD_ID "v0.1beta1")
add_compile_definitions(BUILD_ID="${APP_BUILD_ID}")

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # ⚠️ 重要：Windows 构建器架构配置
    # 当前构建器：${CMAKE_GENERATOR_PLATFORM} (通常为 x86 或 x64)
    # 如果遇到 HDF5 链接错误，说明构建器与 HDF5 库架构不匹配
    # 解决方案：
    #   方案 A：重建目录时指定正确的架构生成器，如：
    #     cmake -B build -G "Visual Studio 17 2022" -A x64 -DOPENCV_ROOT="..."
    #   方案 B：禁用 HDF5 模块：-DBUILD_opencv_hdf=OFF
    # 详见 CREDITS.md#缺失（❌）清单
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(BUILD_WITH_STATIC_CRT OFF CACHE BOOL "" FORCE)
endif()

option(RK_ENABLE_NCNN "启用 ncnn 后端（优先 find_package，缺失时可选 FetchContent）" OFF)
option(RK_NCNN_FETCHCONTENT "当 ncnn 未安装时，允许通过 FetchContent 拉取并构建 ncnn" OFF)
set(RK_NCNN_GIT_REPOSITORY "https://github.com/Tencent/ncnn.git" CACHE STRING "ncnn 仓库地址（仅 FetchContent 时使用）")
set(RK_NCNN_GIT_TAG "master" CACHE STRING "ncnn 版本（tag/commit，默认 master；仅 FetchContent 时使用）")

option(RK_ENABLE_MPP "启用 RK MPP 硬解码加速（默认开启，缺失依赖时自动回退为占位实现）" ON)
option(RK_ENABLE_QUALCOMM "启用 Qualcomm SDK 推理加速（默认开启，缺失依赖时自动回退为占位实现）" ON)

if(RK_ENABLE_MPP)
    # ⚠️ 注意：RK MPP 已下载至 D:\ProgramData\rkmpp\mpp-1.0.11
    # 配置步骤（二选一）：
    # 方案 A（推荐）：设置环境变量 RK_MPP_HOME
    #   set RK_MPP_HOME=D:\ProgramData\rkmpp\mpp-1.0.11
    #   重新运行 CMake 配置
    # 方案 B：复制或链接文件到 deps/rk_mpp/
    #   xcopy D:\ProgramData\rkmpp\mpp-1.0.11\inc deps\rk_mpp\include /E /I
    #   或 mklink /D deps\rk_mpp D:\ProgramData\rkmpp\mpp-1.0.11 (需管理员权限)
    find_path(RK_MPP_INCLUDE_DIR rk_mpi.h 
        PATHS 
            ${CMAKE_CURRENT_SOURCE_DIR}/deps/rk_mpp/include
            ${CMAKE_CURRENT_SOURCE_DIR}/deps/rk_mpp/inc
            $ENV{RK_MPP_HOME}/include
            $ENV{RK_MPP_HOME}/inc
        NO_DEFAULT_PATH)
    if(RK_MPP_INCLUDE_DIR)
        set(RK_HAVE_MPP 1)
        include_directories(${RK_MPP_INCLUDE_DIR})
        message(STATUS "✅ Found RK MPP headers in ${RK_MPP_INCLUDE_DIR}")
    else()
        set(RK_HAVE_MPP 0)
        message(WARNING "❌ RK MPP headers not found. Falling back to CPU decoding. (See CREDITS.md#可选)")
    endif()
else()
    set(RK_HAVE_MPP 0)
endif()

if(RK_ENABLE_QUALCOMM)
    # ⚠️ 注意：Qualcomm SNPE SDK 未检测到（可选）
    # 如需启用，请：
    # 1. 从 Qualcomm 官方获取 SNPE SDK
    # 2. 解压至 deps/qualcomm_snpe/
    # 3. 重新运行 CMake 配置
    find_path(QUALCOMM_INCLUDE_DIR DlSystem/DlError.hpp 
        PATHS 
            ${CMAKE_CURRENT_SOURCE_DIR}/deps/qualcomm_snpe/include
            $ENV{QUALCOMM_HOME}/include
        NO_DEFAULT_PATH)
    if(QUALCOMM_INCLUDE_DIR)
        set(RK_HAVE_QUALCOMM 1)
        include_directories(${QUALCOMM_INCLUDE_DIR})
        message(STATUS "Found Qualcomm SDK headers in ${QUALCOMM_INCLUDE_DIR}")
    else()
        set(RK_HAVE_QUALCOMM 0)
        message(WARNING "❌ Qualcomm SDK headers not found. Falling back to CPU inference. (See CREDITS.md)")
    endif()
else()
    set(RK_HAVE_QUALCOMM 0)
endif()

add_definitions(-DRK_HAVE_MPP=${RK_HAVE_MPP} -DRK_HAVE_QUALCOMM=${RK_HAVE_QUALCOMM})

if(ANDROID)
    set(_rk_enable_libyuv_default ON)
else()
    set(_rk_enable_libyuv_default OFF)
endif()
option(RK_ENABLE_LIBYUV "启用 libyuv（用于 YUV->BGR 等像素转换加速；缺省 Android=ON / Host=OFF）" ${_rk_enable_libyuv_default})

# Optimization flags
# -mcpu=cortex-a17 and -mfpu=neon-vfpv4 are specific to the RK3288 (armeabi-v7a)
# We only apply them when targeting that architecture to avoid errors on x86_64 or arm64-v8a.
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mcpu=cortex-a17 -mfpu=neon-vfpv4 -ftree-vectorize")
endif()

# Security Hardening Flags (Production Grade)
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GS")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,relro -Wl,-z,now")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,relro -Wl,-z,now -fPIE -pie")
endif()

# Include directories
include_directories(src/cpp/include)

option(RK_SKIP_OPENCV "跳过 OpenCV 构建（仅可用于不依赖 OpenCV 的目标，如 core_unit_tests）" OFF)
option(RK_BUILD_CORE_UNIT_TESTS "Build core unit tests" ON)

if(NOT ANDROID AND RK_BUILD_CORE_UNIT_TESTS)
    enable_testing()
    add_executable(core_unit_tests
        "tests/cpp/core_unit_tests_main.cpp"
        "tests/cpp/test_frame_input_channel.cpp"
        "tests/cpp/test_face_search.cpp"
        "tests/cpp/test_threshold_policy.cpp"
        "tests/cpp/test_event_manager.cpp"
        "src/cpp/src/FrameInputChannel.cpp"
        "src/cpp/src/FaceSearch.cpp"
        "src/cpp/src/ThresholdPolicy.cpp"
        "src/cpp/src/EventManager.cpp"
        "tests/win/test_http_faces_server.cpp"
        "src/win/src/HttpFacesServerPath.cpp"
    )
    target_include_directories(core_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
    )
    target_include_directories(core_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    )
    target_compile_definitions(core_unit_tests PRIVATE RK_SKIP_OPENCV=1)
    if(MSVC)
        target_compile_options(core_unit_tests PRIVATE /utf-8)
    endif()
    add_test(NAME core_unit_tests COMMAND core_unit_tests)
endif()

if(NOT RK_SKIP_OPENCV)

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
# Explicitly whitelist modules if needed, or rely on default dependencies
# We definitely need: core, imgproc, video, objdetect, face, imgcodecs
set(BUILD_opencv_world OFF CACHE BOOL "Build opencv_world")

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

set(RK_HAVE_NCNN OFF)
set(RK_NCNN_TARGET "")
if(RK_ENABLE_NCNN)
    if(NOT DEFINED NCNN_DIR AND DEFINED ENV{NCNN_DIR})
        set(NCNN_DIR $ENV{NCNN_DIR})
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

set(RK_HAVE_LIBYUV OFF)
set(RK_LIBYUV_TARGET "")
if(RK_ENABLE_LIBYUV)
    include(FetchContent)
    FetchContent_Declare(
        libyuv
        GIT_REPOSITORY https://chromium.googlesource.com/libyuv/libyuv
        GIT_TAG        d9681c53b3af633ab3c64655fcb9625e364b8f9c
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

# Collect Core Source Files (excluding entry points)
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "src/cpp/src/*.cpp"
)

if(ANDROID)
    # 1. Android JNI Shared Library
    if(NOT RK_SKIP_OPENCV)
        add_library(native-lib SHARED ${CORE_SOURCES} "src/cpp/native-lib.cpp")
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
        )
    else()
        add_library(native-lib SHARED ${CORE_SOURCES} "src/cpp/native-lib-stub.cpp")
    endif()

    target_link_libraries(
        native-lib
        $<$<BOOL:${RK_HAVE_LIBYUV}>:${RK_LIBYUV_TARGET}>
        ${log-lib}
        ${jnigraphics-lib}
        android
    )

    # Explicitly set include directories for native-lib
    target_include_directories(native-lib PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    )

    target_compile_definitions(native-lib PRIVATE RK_HAVE_NCNN=$<IF:$<BOOL:${RK_HAVE_NCNN}>,1,0>)
    target_compile_definitions(native-lib PRIVATE RK_HAVE_LIBYUV=$<IF:$<BOOL:${RK_HAVE_LIBYUV}>,1,0>)
    if(RK_HAVE_LIBYUV AND libyuv_SOURCE_DIR)
        target_include_directories(native-lib PRIVATE ${libyuv_SOURCE_DIR}/include)
    endif()
    if(RK_HAVE_NCNN AND RK_NCNN_TARGET)
        target_link_libraries(native-lib ${RK_NCNN_TARGET})
    endif()
endif()

if(NOT RK_SKIP_OPENCV)
    # 2. Native Executable (CLI) for RK3288 (and Host Testing)
    add_executable(rk3288_cli "src/cpp/main.cpp" ${CORE_SOURCES})

    # Explicitly set include directories for rk3288_cli
    target_include_directories(rk3288_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    )

    target_compile_definitions(rk3288_cli PRIVATE RK_HAVE_NCNN=$<IF:$<BOOL:${RK_HAVE_NCNN}>,1,0>)
    target_compile_definitions(rk3288_cli PRIVATE RK_HAVE_LIBYUV=$<IF:$<BOOL:${RK_HAVE_LIBYUV}>,1,0>)
    if(RK_HAVE_LIBYUV AND libyuv_SOURCE_DIR)
        target_include_directories(rk3288_cli PRIVATE ${libyuv_SOURCE_DIR}/include)
    endif()

    # Link Libraries for CLI
    if(ANDROID)
        target_link_libraries(
            rk3288_cli
            opencv_core
            opencv_imgproc
            opencv_video
            opencv_objdetect
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
            $<$<BOOL:${RK_HAVE_LIBYUV}>:${RK_LIBYUV_TARGET}>
            ${log-lib}
            ${jnigraphics-lib}
            android
        )
    else()
        # Host Build (Windows/Linux)
        target_link_libraries(
            rk3288_cli
            opencv_core
            opencv_imgproc
            opencv_video
            opencv_objdetect
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
            $<$<BOOL:${RK_HAVE_LIBYUV}>:${RK_LIBYUV_TARGET}>
            # No android/log libs on host
        )
    endif()

    if(RK_HAVE_NCNN AND RK_NCNN_TARGET)
        target_link_libraries(rk3288_cli ${RK_NCNN_TARGET})
    endif()

    if(TARGET opencv_face)
        target_link_libraries(rk3288_cli opencv_face)
    endif()

    # 3. OpenCV Verification CLI (static image validation)
    add_executable(opencv_verify_cli "src/cpp/opencv_verify_cli.cpp")

    if(ANDROID)
        target_link_libraries(
            opencv_verify_cli
            opencv_core
            opencv_imgproc
            opencv_imgcodecs
            opencv_objdetect
            opencv_features2d
            ${log-lib}
            android
        )
    else()
        target_link_libraries(
            opencv_verify_cli
            opencv_core
            opencv_imgproc
            opencv_imgcodecs
            opencv_objdetect
            opencv_features2d
        )
    endif()
endif()

if(NOT RK_SKIP_OPENCV)
    add_executable(inference_bench_cli "src/cpp/tools/inference_bench_cli.cpp")
    target_include_directories(inference_bench_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    )
    if(MSVC)
        target_compile_options(inference_bench_cli PRIVATE /utf-8)
    endif()
    if(WIN32)
        target_compile_definitions(inference_bench_cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    target_compile_definitions(inference_bench_cli PRIVATE RK_HAVE_NCNN=$<IF:$<BOOL:${RK_HAVE_NCNN}>,1,0>)

    if(ANDROID)
        target_link_libraries(
            inference_bench_cli
            opencv_core
            opencv_imgproc
            opencv_dnn
            ${log-lib}
            android
        )
    else()
        target_link_libraries(
            inference_bench_cli
            opencv_core
            opencv_imgproc
            opencv_dnn
        )
    endif()

    if(RK_HAVE_NCNN AND RK_NCNN_TARGET)
        target_link_libraries(inference_bench_cli ${RK_NCNN_TARGET})
    endif()

    if(WIN32)
        target_link_libraries(inference_bench_cli psapi)
    endif()
endif()

option(RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION "Build Windows Camera Face Recognition Test System" ON)
option(RK_BUILD_WINDOWS_LOCAL_WEB_SERVICE "Build Windows Local Web Service (no Win32 UI)" ON)
option(RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION_UI "Build legacy Win32 UI app (rollback only)" OFF)

if(WIN32 AND RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION)
    if(RK_BUILD_WINDOWS_LOCAL_WEB_SERVICE)
        add_executable(win_local_service
            "src/win/app/win_local_service_main.cpp"
            "src/win/src/WinConfig.cpp"
            "src/win/src/JsonLite.cpp"
            "src/win/src/WinCrypto.cpp"
            "src/win/src/WinJsonConfig.cpp"
            "src/win/src/StructuredLogger.cpp"
            "src/win/src/MfCamera.cpp"
            "src/win/src/FaceDetector.cpp"
            "src/win/src/LbphEmbedder.cpp"
            "src/win/src/FaceDatabase.cpp"
            "src/win/src/FaceRecognizer.cpp"
            "src/win/src/DnnSsdFaceDetector.cpp"
            "src/win/src/OverlayRenderer.cpp"
            "src/win/src/FacesJson.cpp"
            "src/win/src/HttpFacesServer.cpp"
            "src/win/src/HttpFacesServerPath.cpp"
            "src/win/src/HttpFacesPoster.cpp"
            "src/win/src/FramePipeline.cpp"
            "src/win/src/EventLogger.cpp"
            "src/cpp/src/FileHash.cpp"
        )

        target_include_directories(win_local_service PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
        )

        target_compile_definitions(win_local_service PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(win_local_service PRIVATE /utf-8)
        endif()

        target_link_libraries(win_local_service
            opencv_core
            opencv_imgproc
            opencv_objdetect
            opencv_imgcodecs
            opencv_dnn
            mfplat
            mfreadwrite
            mfuuid
            mf
            ole32
            oleaut32
            shlwapi
            shell32
            psapi
            ws2_32
            winhttp
            bcrypt
            crypt32
        )

        add_custom_command(TARGET win_local_service POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/win/app/webroot
                    $<TARGET_FILE_DIR:win_local_service>/webroot
            COMMENT "Copy webroot for local HTTP static hosting"
        )

        add_custom_command(TARGET win_local_service POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:win_local_service>/assets
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/assets/lbpcascade_frontalface.xml
                    $<TARGET_FILE_DIR:win_local_service>/assets/lbpcascade_frontalface.xml
            COMMENT "Copy default cascade asset for Windows local service"
        )

        add_custom_command(TARGET win_local_service POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:win_local_service>/config
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${CMAKE_CURRENT_SOURCE_DIR}/config/windows_camera_face_recognition.ini
                    $<TARGET_FILE_DIR:win_local_service>/config/windows_camera_face_recognition.ini
            COMMENT "Copy default ini (migration source) for Windows local service"
        )
    endif()

    if(RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION_UI)
        add_executable(win_camera_face_recognition WIN32
            "src/win/app/win_camera_face_recognition_main.cpp"
            "src/win/src/WinConfig.cpp"
            "src/win/src/JsonLite.cpp"
            "src/win/src/WinCrypto.cpp"
            "src/win/src/WinJsonConfig.cpp"
            "src/win/src/StructuredLogger.cpp"
            "src/win/src/MfCamera.cpp"
            "src/win/src/FaceDetector.cpp"
            "src/win/src/LbphEmbedder.cpp"
            "src/win/src/FaceDatabase.cpp"
            "src/win/src/FaceRecognizer.cpp"
            "src/win/src/DnnSsdFaceDetector.cpp"
            "src/win/src/OverlayRenderer.cpp"
            "src/win/src/FacesJson.cpp"
            "src/win/src/HttpFacesServer.cpp"
            "src/win/src/HttpFacesServerPath.cpp"
            "src/win/src/HttpFacesPoster.cpp"
            "src/win/src/AbcTestRunner.cpp"
            "src/win/src/FramePipeline.cpp"
            "src/win/src/DisplaySettings.cpp"
            "src/win/src/D3D11Renderer.cpp"
            "src/win/src/RenderMetricsLogger.cpp"
            "src/win/src/EventLogger.cpp"
        )

        target_include_directories(win_camera_face_recognition PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
        )

        target_compile_definitions(win_camera_face_recognition PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(win_camera_face_recognition PRIVATE /utf-8)
        endif()

        target_link_libraries(win_camera_face_recognition
            opencv_core
            opencv_imgproc
            opencv_objdetect
            opencv_imgcodecs
            opencv_dnn
            mfplat
            mfreadwrite
            mfuuid
            mf
            ole32
            oleaut32
            shlwapi
            comctl32
            user32
            gdi32
            shell32
            d3d11
            dxgi
            dxguid
            d3dcompiler
            psapi
            ws2_32
            winhttp
            bcrypt
            crypt32
        )

        add_custom_command(TARGET win_camera_face_recognition POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/win/app/webroot
                    $<TARGET_FILE_DIR:win_camera_face_recognition>/webroot
            COMMENT "Copy webroot for local HTTP static hosting"
        )
    endif()

    add_executable(win_face_eval_cli
        "src/win/tools/win_face_eval_cli.cpp"
        "src/win/src/WinConfig.cpp"
        "src/win/src/JsonLite.cpp"
        "src/win/src/WinCrypto.cpp"
        "src/win/src/WinJsonConfig.cpp"
        "src/win/src/FaceDetector.cpp"
        "src/win/src/LbphEmbedder.cpp"
        "src/win/src/FaceDatabase.cpp"
        "src/win/src/FaceRecognizer.cpp"
        "src/win/src/StructuredLogger.cpp"
    )
    target_include_directories(win_face_eval_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    )
    target_compile_definitions(win_face_eval_cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(win_face_eval_cli PRIVATE /utf-8)
    endif()
    target_link_libraries(win_face_eval_cli
        opencv_core
        opencv_imgproc
        opencv_objdetect
        opencv_imgcodecs
        shlwapi
        shell32
        bcrypt
        crypt32
    )

    add_executable(win_face_bench_cli
        "src/win/tools/win_face_bench_cli.cpp"
        "src/win/src/WinConfig.cpp"
        "src/win/src/JsonLite.cpp"
        "src/win/src/WinCrypto.cpp"
        "src/win/src/WinJsonConfig.cpp"
        "src/win/src/FaceDetector.cpp"
        "src/win/src/LbphEmbedder.cpp"
        "src/win/src/FaceDatabase.cpp"
        "src/win/src/FaceRecognizer.cpp"
    )
    target_include_directories(win_face_bench_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    )
    target_compile_definitions(win_face_bench_cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(win_face_bench_cli PRIVATE /utf-8)
    endif()
    target_link_libraries(win_face_bench_cli
        opencv_core
        opencv_imgproc
        opencv_objdetect
        opencv_imgcodecs
        psapi
        bcrypt
        crypt32
    )

    enable_testing()
    add_executable(win_unit_tests
        "tests/win/win_unit_tests_main.cpp"
        "tests/win/test_face_metrics.cpp"
        "tests/win/test_lbph_embedder.cpp"
        "tests/win/test_http_faces_server.cpp"
        "src/win/src/HttpFacesServerPath.cpp"
        "src/win/src/LbphEmbedder.cpp"
        "src/win/src/FaceRecognizer.cpp"
        "src/win/src/FaceDatabase.cpp"
        "src/win/src/FaceDetector.cpp"
        "src/win/src/WinConfig.cpp"
        "src/win/src/JsonLite.cpp"
        "src/win/src/WinCrypto.cpp"
        "src/win/src/WinJsonConfig.cpp"
    )
    target_include_directories(win_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    )
    target_compile_definitions(win_unit_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(win_unit_tests PRIVATE /utf-8)
    endif()
    target_link_libraries(win_unit_tests
        opencv_core
        opencv_imgproc
        opencv_objdetect
        opencv_imgcodecs
        bcrypt
        crypt32
    )
    add_test(NAME win_unit_tests COMMAND win_unit_tests)
endif()

endif()
