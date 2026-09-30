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
    inference_bench_cli win_face_eval_cli win_face_bench_cli win_replay_export_cli
    win_unit_tests win_face_database_perf
)

# ── 基准预算覆盖（§10.11 D4b）────────────────────────
# 覆盖 RealtimeBenchmarkConfig.enforceBudget 的平台默认值。
# ★ 留空 = **不注入**该宏 ⇒ .cpp 的 kDefaultEnforceBudget 回落到 __aarch64__ 判定 ⇒ **零行为变化**
#   （唯测试 target `core_unit_tests` 例外，见 `cmake/core.cmake`）。
#   此处刻意不用布尔 option(OFF)：固定 OFF 会静默改变 Android arm64 行为（超预算从「拒绝发布」变「照发」）。
set(RK_BENCH_ENFORCE_BUDGET "" CACHE STRING "强制/关闭基准预算判定；留空 = 按架构回落（见 RealtimeFacePipeline.cpp）")
set_property(CACHE RK_BENCH_ENFORCE_BUDGET PROPERTY STRINGS "" ON OFF)

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
    ${RK_CORE_LITE_SOURCES} ${RK_FACE_INFER_CORE_SOURCES} ${RK_ADAPTER_SOURCES} ${RK_PIPELINE_SOURCES}
    "src/win/src/FaceDetector.cpp"
    "src/win/src/LbphEmbedder.cpp"
    "src/win/src/DnnSsdFaceDetector.cpp"
    "src/win/src/JsonLite.cpp"
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
        "tests/cpp/test_model_registry.cpp"
        "tests/cpp/test_face_align.cpp"
        "tests/cpp/test_resource_cleanup.cpp"
        "tests/cpp/test_gallery_cache.cpp"
        "tests/cpp/test_result_publisher.cpp"
        ${RK_CORE_LITE_SOURCES} ${RK_FACE_INFER_CORE_SOURCES} ${RK_ADAPTER_SOURCES}
        "src/win/src/DnnSsdFaceDetector.cpp"
        "src/win/src/FaceDetector.cpp"
        "src/win/src/LbphEmbedder.cpp"
        "src/win/src/JsonLite.cpp"
        "src/cpp/src/pipeline/ResultPublisher.cpp"
        "src/cpp/src/pipeline/PerfReporter.cpp"
        "src/cpp/src/pipeline/TrackCoordinator.cpp"
        "src/cpp/src/pipeline/FacePostProcessor.cpp"
        "src/cpp/src/pipeline/ScrfdDecoder.cpp"
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
    target_compile_definitions(face_infer_unit_tests PRIVATE
            RK_HAVE_NCNN=$<IF:$<BOOL:${RK_HAVE_NCNN}>,1,0>)
    if(WIN32)
        target_compile_definitions(face_infer_unit_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN RK_HAVE_MPP=0)
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
    # 源码根作为 WORKING_DIRECTORY：test_mock_preflight_guards.cpp 的 findFixture 依赖
    # 相对路径 tests/fixtures/mock/，其候选路径仅两级（cwd 与 cwd.parent_path()）——
    # build 为源码根直接子目录时 parent_path() 兜底本可命中；仅两级以上的 out-of-tree
    # build 才会解析失败。设源码根使 fixture 契约独立于 build 位置（B15-001）。
    add_test(NAME face_infer_unit_tests COMMAND face_infer_unit_tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # 超时保护：与 win_unit_tests 同策略 —— 该 target 内若有阻塞会无限挂起整个 ctest，
    # 故设 120s 把「挂起」降级为一次可观测的 Timeout 失败。
    # ⚠️ 归因订正（2026-09-28，与 cmake/windows.cmake 的同名订正配套）：原注释把成因写作
    #    「（B06-014）」—— **B06-014 实为 FramePipeline 运行时所有权并发的 UAF**（模型热重载
    #    释放处理线程正在使用的对象），与本处挂起无关；本超时实为**模板式复制**。
    #    ⇒ 两处注释此前互相矛盾（windows.cmake 已改、此处未改），现已同步。
    # ⚠️ 数值**不再与 `win_unit_tests` 同值**（2026-09-28 用户拍板）：那边因「负载下正常时长可达
    #    ~118s」已上调 **120 → 180**；本 target 正常时长 **~1.5s**、120s 余量充足 ⇒ **保持 120 不变**。
    #    勿再因「同策略」把两者改齐。
    set_tests_properties(face_infer_unit_tests PROPERTIES TIMEOUT 120)
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
    # B06-005：googletest 已由 cmake/core.cmake:49-53 在 RK_BUILD_CORE_UNIT_TESTS 分支下
    # FetchContent 构建（GTest::gtest 已存在）。此处仅在 core.cmake 未提供时兜底声明，
    # 避免同一 configure 内对已填充依赖重复 Declare/MakeAvailable。
    # GIT_REPOSITORY/GIT_TAG 与 cmake/core.cmake 保持一致（版本统一由 RK_GTEST_GIT_TAG 控制，RW8 ⑤）。
    if(NOT TARGET GTest::gtest)
        include(FetchContent)
        # 与 cmake/core.cmake:47 一致（RW8 ④）：禁用系统预编译 GTest（Anaconda 等用动态
        # CRT，与项目静态 /MT 不匹配，链接报 LNK2038 RuntimeLibrary 不匹配 / LNK2005）
        set(CMAKE_DISABLE_FIND_PACKAGE_GTest ON CACHE BOOL "" FORCE)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG ${RK_GTEST_GIT_TAG})
        if(MSVC)
            # 与 cmake/core.cmake:50 门控一致（RW8 ②）：gtest_force_shared_crt 仅影响
            # MSVC CRT（OFF → 静态 /MT）；Linux 下属噪音，不 FORCE
            set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
        endif()
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(core_gtest_tests
        "tests/cpp/test_engine.cpp" "tests/cpp/test_bioauth.cpp"
        "tests/cpp/test_motion.cpp" "tests/cpp/test_adapters.cpp"
        "tests/cpp/test_arcface_contract.cpp"
        "tests/cpp/test_face_runtime_profile.cpp"
        "tests/cpp/test_scrfd_decoder.cpp"
        "tests/cpp/test_scrfd_ncnn_adapter.cpp"
        "tests/cpp/test_frame_transform.cpp"
        "src/win/src/JsonLite.cpp"
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
    target_compile_definitions(core_gtest_tests PRIVATE
        RK_HAVE_NCNN=$<IF:$<BOOL:${RK_HAVE_NCNN}>,1,0>)
    if(WIN32)
        target_compile_definitions(core_gtest_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN RK_HAVE_MPP=0)
    endif()
    # 测试接缝编译期开关（第四轮审查 M3）：仅本 target 需要——它编译 test_engine.cpp 的
    # BarrierEngine（覆写 Engine::onBeforeRenderPublish）且在同一 target 内编译 Engine.cpp，
    # 宏对二者一致故无 ODR/布局不一致；生产构建不定义 → Engine 无虚函数、无 vptr（ABI 稳定）。
    # 不全局定义：core_unit_tests 等 target 不编译 test_engine.cpp/Engine.cpp，无需该宏。
    target_compile_definitions(core_gtest_tests PRIVATE RK_ENABLE_TEST_SEAMS)
    # §10.11 D4b：基准预算覆盖宏。core_gtest_tests 同 /EHsc、RK_ENABLE_TEST_SEAMS 一样**不走公共
    # 循环、也不链接 rk_core**，故在此单独补注入（它同样经 ${RK_FACE_INFER_CORE_SOURCES} 编译
    # RealtimeFacePipeline.cpp）。留空 = 不注入 ⇒ 零行为变化（见上方 option 注释）。
    if(NOT RK_BENCH_ENFORCE_BUDGET STREQUAL "")
        target_compile_definitions(core_gtest_tests PRIVATE
            "RK_BENCH_ENFORCE_BUDGET=$<BOOL:${RK_BENCH_ENFORCE_BUDGET}>")
    endif()
    # test_arcface_contract.cpp::LoadEmbedderConsumesManifestEmbedderBinWiring 的接线守卫需读仓内
    # 源码文本（src/cpp/src/pipeline/RealtimeFacePipeline.cpp）⇒ 必须定义源码根（与 core_unit_tests
    # 同口径，见 cmake/core.cmake:116）。该守卫不依赖模型/OpenCV/ncnn，故 CI 恒跑（windows job 的
    # `ctest -R "unit_tests|core_gtest_tests"`）。缺此定义时该文件的 `#else #error` 会让构建**响亮失败**
    # （而非让守卫静默消失）——范式同 test_log_rate_limiter.cpp / test_track_coordinator.cpp。
    target_compile_definitions(core_gtest_tests PRIVATE RK_TEST_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(core_gtest_tests GTest::gtest_main ${RK_OPENCV_FULL_LIBS})
    if(RK_HAVE_NCNN AND RK_NCNN_TARGET)
        target_link_libraries(core_gtest_tests ${RK_NCNN_TARGET})
    endif()
    # 源码根作为 WORKING_DIRECTORY —— 双代理审计（A1/A2）+ 子代理 3/4 复核共同定位的缺口：
    # `test_adapters.cpp::MobileFaceNetDefaultsMatchShippedModelFiles`（本批新增）用**相对路径**
    # `models/...` 判断模型是否存在。不设本属性时 `ctest` 从**构建目录**起算 ⇒ 它在 ctest 下
    # **静默 GTEST_SKIP**，本机这条守卫形同虚设（已实测：设属性前 ctest 下 SKIP、设属性后 OK）。
    # ⚠️ **修好 cwd 只解决一半，不要把这条读成「CI 已守住」**（子代理 4 S1 与子代理 3 各自独立指出）：
    #    `models/*` 是 gitignored 的二进制（`git ls-files models/` 仅 7 个文本文件、无任何 `.param`），
    #    CI 全新检出后**这些模型根本不存在** ⇒ 即便 cwd 正确，该用例在 CI 上**仍会 SKIP**。
    #    ⇒ 「适配器默认值 ↔ **真实模型**」这条对齐校验**本质上只能在有模型的本机跑**。
    #    CI 侧真正守住的是它的**另一半**：`MobileFaceNetNodeNamesAndDimAreConfigurable`
    #    （无任何跳过条件、恒运行）把默认值钉死为 in0/out0/512 ⇒ **常量漂移在 CI 可被拦**。
    # ⚠️ 同处更正（子代理 3/4 各指出一次）：`AdapterLoadsRealInt8ModelWithInjectedNames` **不是**因 cwd
    #    才跳过 —— 它在本机（模型存在）仍 SKIP，原因是 `RK_HAVE_NCNN=0`（本机构建与 CI 均未启用 ncnn）。
    #    修 cwd 对它是必要不充分；它真正缺的前置是「一个启用 ncnn 的构建」。
    # ⚠️ 另一处更正：「本 target 其余用例均不依赖 cwd」**不成立**：`AdapterTest.ModelRegistryListAllEmpty`
    #    依赖 `ModelRegistry` 的**惰性注册**，而注册用的正是相对路径 `models/*.param` ⇒ 一旦将来有用例
    #    先触发注册，cwd=源根（本机有模型）就会让该断言翻红。当前无触发者，属**潜在脆弱**，此处留档。
    add_test(NAME core_gtest_tests COMMAND core_gtest_tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

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

# ── 基准预算覆盖注入（§10.11 D4b）────────────────────────
# 仅当 RK_BENCH_ENFORCE_BUDGET 被**显式设置**（ON/OFF 皆算，空串不算）时注入。判据刻意用
# `NOT ... STREQUAL ""` 而非 `if(...)`：后者会把 OFF 当作假值，导致「显式关闭」在 Android arm64
# 上静默失效（超预算仍拒绝发布）。
#   · 主注入点 = rk_core 的 INTERFACE 定义：rk_core 是 INTERFACE 库，链它的 target 自动获得；
#     本仓其唯一消费者 core_unit_tests 正好编译 RealtimeFacePipeline.cpp。
#   · 以下 target 同样编译 RealtimeFacePipeline.cpp，却**不链接 rk_core**，故显式补注入：
#     rk3288_cli / face_infer_unit_tests（本文件）与 native-lib（cmake/android.cmake，仅 Android 存在）。
#   · core_gtest_tests 已在其实义处单独注入（它不走公共循环）。
# 统一性由 rk_assert_bench_budget_uniformity() 在配置期强制。
if(NOT RK_BENCH_ENFORCE_BUDGET STREQUAL "")
    target_compile_definitions(rk_core INTERFACE
        "RK_BENCH_ENFORCE_BUDGET=$<BOOL:${RK_BENCH_ENFORCE_BUDGET}>")
    foreach(t rk3288_cli face_infer_unit_tests native-lib)
        if(TARGET ${t})
            target_compile_definitions(${t} PRIVATE
                "RK_BENCH_ENFORCE_BUDGET=$<BOOL:${RK_BENCH_ENFORCE_BUDGET}>")
        endif()
    endforeach()
endif()

# 编译器契约 — 在所有 target 定义之后统一应用
rk_apply_compiler_contract()
# 配置期断言（RW8 ①）— 契约应用后独立读回 COMPILE_OPTIONS，验证 flag 已落到 target
rk_assert_contract_flags()
# 配置期断言（FACE-CLEAN-019）— 锁定测试接缝宏只出现在允许的 target 上（两个方向都查）
rk_assert_test_seam_boundary()
# 配置期断言（§10.8）— 锁定测试接缝宏 RK_ENABLE_TRACK_RESET_SEAM 只出现在 core_unit_tests 上
rk_assert_track_reset_seam_boundary()
# 配置期断言（§10.11 D4b）— 凡编译 RealtimeFacePipeline.cpp 的 target 必须统一携带基准预算宏
rk_assert_bench_budget_uniformity()
