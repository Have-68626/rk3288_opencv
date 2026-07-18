cmake_minimum_required(VERSION 3.18.1)
project(RK3288_AI_Engine)

# Version definition for Windows/CLI
set(APP_BUILD_ID "v0.1beta1")
add_compile_definitions(BUILD_ID="${APP_BUILD_ID}")

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ccache (build cache, auto-detect when available)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache detected: ${CCACHE_PROGRAM}")
endif()

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

# ── Options ──────────────────────────────────────────
option(RK_ENABLE_NCNN "启用 ncnn 后端（优先 find_package，缺失时可选 FetchContent）" OFF)
option(RK_NCNN_FETCHCONTENT "当 ncnn 未安装时，允许通过 FetchContent 拉取并构建 ncnn" OFF)
set(RK_NCNN_GIT_REPOSITORY "https://github.com/Tencent/ncnn.git" CACHE STRING "ncnn 仓库地址（仅 FetchContent 时使用）")
set(RK_NCNN_GIT_TAG "master" CACHE STRING "ncnn 版本（tag/commit，默认 master；仅 FetchContent 时使用）")

option(RK_ENABLE_MPP "启用 RK MPP 硬解码加速（默认开启，缺失依赖时自动回退为占位实现）" ON)
option(RK_ENABLE_QUALCOMM "启用 Qualcomm SDK 推理加速（默认开启，缺失依赖时自动回退为占位实现）" ON)

option(RK_SKIP_OPENCV "跳过 OpenCV 构建（仅可用于不依赖 OpenCV 的目标，如 core_unit_tests）" OFF)
option(RK_BUILD_CORE_UNIT_TESTS "Build core unit tests" ON)

option(RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION "Build Windows Camera Face Recognition Test System" ON)
option(RK_BUILD_WINDOWS_LOCAL_WEB_SERVICE "Build Windows Local Web Service (no Win32 UI)" ON)
option(RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION_UI "Build legacy Win32 UI app (rollback only)" OFF)

# ── MPP 检测 ──────────────────────────────────────
if(RK_ENABLE_MPP)
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
        message(STATUS "Found RK MPP headers in ${RK_MPP_INCLUDE_DIR}")
    else()
        set(RK_HAVE_MPP 0)
        message(WARNING "RK MPP headers not found. Falling back to CPU decoding. (See CREDITS.md#optional)")
    endif()
else()
    set(RK_HAVE_MPP 0)
endif()

# ── Qualcomm 检测 ─────────────────────────────────
if(RK_ENABLE_QUALCOMM)
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
        message(WARNING "Qualcomm SDK headers not found. Falling back to CPU inference. (See CREDITS.md)")
    endif()
else()
    set(RK_HAVE_QUALCOMM 0)
endif()

set(RK_COMMON_DEFS RK_HAVE_MPP=${RK_HAVE_MPP} RK_HAVE_QUALCOMM=${RK_HAVE_QUALCOMM})

# Collect non-test engine target names for per-target include/dep application
set(RK_ENGINE_TARGETS
    core_unit_tests rk3288_cli opencv_verify_cli native-lib
    win_local_service win_camera_face_recognition
    face_infer_unit_tests ncnn_precision_test
    inference_bench_cli win_face_eval_cli win_face_bench_cli
    win_unit_tests win_face_database_perf
)

# ── libyuv 选项 ────────────────────────────────────
if(ANDROID)
    set(_rk_enable_libyuv_default ON)
else()
    set(_rk_enable_libyuv_default OFF)
endif()
option(RK_ENABLE_LIBYUV "启用 libyuv（用于 YUV->BGR 等像素转换加速；缺省 Android=ON / Host=OFF）" ${_rk_enable_libyuv_default})

# ── 平台专用 flags ────────────────────────────────
# -mcpu=cortex-a17 and -mfpu=neon-vfpv4 are specific to the RK3288 (armeabi-v7a)
# We only apply them when targeting that architecture to avoid errors on x86_64 or arm64-v8a.
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mcpu=cortex-a17 -mfpu=neon-vfpv4 -ftree-vectorize")
endif()

# ── Security Hardening ──────────────────────────────
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GS")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,relro -Wl,-z,now")
    if(UNIX AND NOT APPLE)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,relro -Wl,-z,now -fPIE -pie")
    else()
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,relro -Wl,-z,now")
    endif()
endif()

# ── Common includes ─────────────────────────────────
set(RK_COMMON_INCLUDES
    "${CMAKE_SOURCE_DIR}/src/cpp/include"
    "${CMAKE_SOURCE_DIR}/src/cpp/include/adapters"
    "${CMAKE_SOURCE_DIR}/src/win/include"
)

# ═════════════════════════════════════════════════════
# Module includes
# ═════════════════════════════════════════════════════

include(cmake/core.cmake)

if(NOT RK_SKIP_OPENCV)
    include(cmake/opencv.cmake)
    include(cmake/face_infer.cmake)
endif()

if(ANDROID)
    include(cmake/android.cmake)
endif()

if(WIN32 AND NOT RK_SKIP_OPENCV AND RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION)
    include(cmake/windows.cmake)
endif()

# ═════════════════════════════════════════════════════
# Cross-platform OpenCV targets
# ═════════════════════════════════════════════════════

if(NOT RK_SKIP_OPENCV)

# Native Executable (CLI) for RK3288 (and Host Testing)
add_executable(rk3288_cli
    "src/cpp/main.cpp"
    ${RK_CORE_LITE_SOURCES} ${RK_FACE_INFER_CORE_SOURCES} ${RK_ADAPTER_SOURCES}
    "src/win/src/FaceDetector.cpp"
    "src/win/src/LbphEmbedder.cpp"
    "src/win/src/DnnSsdFaceDetector.cpp"
)
target_include_directories(rk3288_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include/adapters
    ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include/rk_win
)
target_compile_definitions(rk3288_cli PRIVATE RK_HAVE_NCNN=$<IF:$<BOOL:${RK_HAVE_NCNN}>,1,0>)
target_compile_definitions(rk3288_cli PRIVATE RK_HAVE_LIBYUV=$<IF:$<BOOL:${RK_HAVE_LIBYUV}>,1,0>)
if(WIN32)
    target_compile_definitions(rk3288_cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(RK_HAVE_LIBYUV AND libyuv_SOURCE_DIR)
    target_include_directories(rk3288_cli PRIVATE ${libyuv_SOURCE_DIR}/include)
endif()
if(ANDROID)
    target_link_libraries(rk3288_cli ${RK_OPENCV_FULL_LIBS}
        $<$<BOOL:${RK_HAVE_LIBYUV}>:${RK_LIBYUV_TARGET}> ${log-lib} ${jnigraphics-lib} android)
else()
    target_link_libraries(rk3288_cli ${RK_OPENCV_FULL_LIBS}
        $<$<BOOL:${RK_HAVE_LIBYUV}>:${RK_LIBYUV_TARGET}>)
endif()
if(RK_HAVE_NCNN AND RK_NCNN_TARGET)
    target_link_libraries(rk3288_cli ${RK_NCNN_TARGET})
endif()

# OpenCV Verification CLI (static image validation)
add_executable(opencv_verify_cli "src/cpp/opencv_verify_cli.cpp")
if(ANDROID)
    target_link_libraries(opencv_verify_cli ${RK_OPENCV_FULL_LIBS} ${log-lib} android)
else()
    target_link_libraries(opencv_verify_cli ${RK_OPENCV_FULL_LIBS})
endif()

# ── GTest 基础测试 ──
option(RK_BUILD_FACE_INFER_UNIT_TESTS "Build face inference unit tests (requires OpenCV)" ON)

if(NOT ANDROID AND RK_BUILD_FACE_INFER_UNIT_TESTS)
    enable_testing()
    add_executable(face_infer_unit_tests
        "tests/cpp/face_infer_unit_tests_main.cpp"
        "tests/cpp/test_face_infer_pipeline.cpp"
        "tests/cpp/test_face_infer_outcome_json.cpp"
        "tests/cpp/test_mock_preflight_guards.cpp"
        "tests/cpp/test_video_manager.cpp"
        "tests/cpp/test_int8_quantization.cpp"
        "tests/cpp/test_face_align.cpp"
        "tests/cpp/test_resource_cleanup.cpp"
        "tests/cpp/test_gallery_cache.cpp"
        "tests/cpp/test_result_publisher.cpp"
        ${RK_CORE_LITE_SOURCES} ${RK_FACE_INFER_CORE_SOURCES} ${RK_ADAPTER_SOURCES}
        "src/win/src/DnnSsdFaceDetector.cpp"
        "src/win/src/FaceDetector.cpp"
        "src/win/src/LbphEmbedder.cpp"
        "src/cpp/src/pipeline/ResultPublisher.cpp"
        "src/cpp/src/pipeline/PerfReporter.cpp"
        "src/cpp/src/pipeline/TrackCoordinator.cpp"
    )
    target_include_directories(face_infer_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include/adapters
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include/rk_win
    )
    if(MSVC)
        target_compile_options(face_infer_unit_tests PRIVATE /utf-8)
    endif()
    if(WIN32)
        target_compile_definitions(face_infer_unit_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN RK_HAVE_NCNN=$<IF:$<BOOL:${RK_HAVE_NCNN}>,1,0> RK_HAVE_MPP=0)
    endif()
    target_link_libraries(face_infer_unit_tests ${RK_OPENCV_FULL_LIBS})
    if(TARGET GTest::gtest)
        target_link_libraries(face_infer_unit_tests GTest::gtest)
    endif()
    if(RK_HAVE_NCNN AND RK_NCNN_TARGET)
        target_link_libraries(face_infer_unit_tests ${RK_NCNN_TARGET})
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models/yolo_face_int8_ncnn/yolo_face_int8.param")
        target_compile_definitions(face_infer_unit_tests PRIVATE RK_HAVE_INT8_MODELS=1)
    endif()
    add_test(NAME face_infer_unit_tests COMMAND face_infer_unit_tests)
endif()

# 独立 ncnn 精度测试：仅链接 ncnn + OpenCV，不含适配器代码
if(NOT ANDROID AND RK_HAVE_NCNN AND RK_NCNN_TARGET)
    add_executable(ncnn_precision_test "tests/cpp/ncnn_precision_test.cpp")
    find_path(_ncnn_include ncnn/net.h PATHS "${NCNN_DIR}/include" NO_DEFAULT_PATH)
    target_include_directories(ncnn_precision_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include ${_ncnn_include})
    target_compile_definitions(ncnn_precision_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(ncnn_precision_test PRIVATE /utf-8)
        set_target_properties(ncnn_precision_test PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    target_link_libraries(ncnn_precision_test ${RK_OPENCV_FULL_LIBS} ${RK_NCNN_TARGET})
    add_test(NAME ncnn_precision_test COMMAND ncnn_precision_test)
endif()

# ── GTest 基础测试（Engine / BioAuth / MotionDetector / ModelRegistry）─────
if(NOT ANDROID)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.1)
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(core_gtest_tests
        "tests/cpp/test_engine.cpp" "tests/cpp/test_bioauth.cpp"
        "tests/cpp/test_motion.cpp" "tests/cpp/test_adapters.cpp"
        ${RK_CORE_LITE_SOURCES} ${RK_FACE_INFER_CORE_SOURCES} ${RK_ADAPTER_SOURCES}
        ${RK_PIPELINE_SOURCES}
        "src/win/src/DnnSsdFaceDetector.cpp" "src/win/src/FaceDetector.cpp"
        "src/win/src/LbphEmbedder.cpp")
    target_include_directories(core_gtest_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include/adapters
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include/rk_win)
    if(MSVC)
        target_compile_options(core_gtest_tests PRIVATE /utf-8)
    endif()
    if(WIN32)
        target_compile_definitions(core_gtest_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN
            RK_HAVE_NCNN=$<IF:$<BOOL:${RK_HAVE_NCNN}>,1,0> RK_HAVE_MPP=0)
    endif()
    target_link_libraries(core_gtest_tests GTest::gtest_main ${RK_OPENCV_FULL_LIBS})
    if(RK_HAVE_NCNN AND RK_NCNN_TARGET)
        target_link_libraries(core_gtest_tests ${RK_NCNN_TARGET})
    endif()
    add_test(NAME core_gtest_tests COMMAND core_gtest_tests)

    # win_unit_tests contains GTest files (test_http_faces_server.cpp), need GTest::gtest (no _main to avoid main() conflict)
    if(WIN32 AND TARGET win_unit_tests AND TARGET GTest::gtest)
        target_link_libraries(win_unit_tests GTest::gtest)
    endif()
endif()

# inference_bench_cli
add_executable(inference_bench_cli "src/cpp/tools/inference_bench_cli.cpp")
target_include_directories(inference_bench_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include)
if(MSVC)
    target_compile_options(inference_bench_cli PRIVATE /utf-8)
endif()
if(WIN32)
    target_compile_definitions(inference_bench_cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_compile_definitions(inference_bench_cli PRIVATE RK_HAVE_NCNN=$<IF:$<BOOL:${RK_HAVE_NCNN}>,1,0>)
if(ANDROID)
    target_link_libraries(inference_bench_cli ${RK_OPENCV_FULL_LIBS} ${log-lib} android)
else()
    target_link_libraries(inference_bench_cli ${RK_OPENCV_FULL_LIBS})
endif()
if(RK_HAVE_NCNN AND RK_NCNN_TARGET)
    target_link_libraries(inference_bench_cli ${RK_NCNN_TARGET})
endif()
if(WIN32)
    target_link_libraries(inference_bench_cli psapi)
endif()

endif() # NOT RK_SKIP_OPENCV

# ═════════════════════════════════════════════════════
# Per-target includes & definitions
# ═════════════════════════════════════════════════════

foreach(t ${RK_ENGINE_TARGETS})
    if(TARGET ${t})
        target_include_directories(${t} PRIVATE ${RK_COMMON_INCLUDES})
        target_compile_definitions(${t} PRIVATE ${RK_COMMON_DEFS})
    endif()
endforeach()
