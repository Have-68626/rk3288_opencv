# ═══════════════════════════════════════════════════════════════════
# core.cmake — rk_core INTERFACE 库 + CORE_SOURCES 变量
# ═══════════════════════════════════════════════════════════════════

# ── CORE_SOURCES 分组变量 ──────────────────────────
set(RK_CORE_LITE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/EventManager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FileHash.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FrameInputChannel.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/ThresholdPolicy.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceSearch.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceTemplate.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/NativeLog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/MppDecoder.cpp
)

# ── Pipeline 依赖（TrackCoordinator/ResultPublisher/PerfReporter）─────
set(RK_PIPELINE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/pipeline/TrackCoordinator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/pipeline/ResultPublisher.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/pipeline/PerfReporter.cpp
)

# rk_core 接口库（无编译对象，仅传播包含路径和编译定义）
# 多个文件（EventManager/FaceInferOutcomeJson 等）通过包含链间接依赖 OpenCV，
# 不能作为 STATIC 库在 SKIP_OPENCV 模式下编译。使用 INTERFACE 库，
# 实际源文件由 core_unit_tests 直接编译（带 Mock OpenCV stubs）。
add_library(rk_core INTERFACE)
target_include_directories(rk_core INTERFACE
    src/cpp/include
    src/cpp/include/adapters
)
target_compile_definitions(rk_core INTERFACE RK_CORE_LIBRARY)

# ── C++ 编译器契约（T9）：仅 Clang/GNU ──
# 注意: rk_core 是 INTERFACE 库，不能在此设置 compile_options。
# -fno-exceptions -fno-rtti -fvisibility=hidden 需要逐 target 应用
# （见 CMakeLists.txt 中各 executable target）

# ── core_unit_tests（无 OpenCV 依赖）──
if(NOT ANDROID AND RK_BUILD_CORE_UNIT_TESTS)
    enable_testing()
    include(FetchContent)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG release-1.12.1
        )
        FetchContent_MakeAvailable(googletest)
    endif()
    add_executable(core_unit_tests
        "tests/cpp/core_unit_tests_main.cpp"
        "tests/cpp/test_frame_input_channel.cpp"
        "tests/cpp/test_face_search.cpp"
        "tests/cpp/test_threshold_policy.cpp"
        "tests/cpp/test_event_manager.cpp"
        "tests/cpp/test_file_hash.cpp"
        "tests/cpp/test_connection_quota.cpp"
        "tests/cpp/test_acceleration_contract.cpp"
        "tests/cpp/test_track_coordinator.cpp"
        "src/cpp/src/pipeline/TrackCoordinator.cpp"
        "src/cpp/src/EventManager.cpp"
        "tests/win/test_http_faces_server.cpp"
        "src/cpp/src/FileHash.cpp"
        "src/cpp/src/FrameInputChannel.cpp"
        "src/cpp/src/ThresholdPolicy.cpp"
        "src/cpp/src/FaceSearch.cpp"
        "src/cpp/src/FaceTemplate.cpp"
        "src/cpp/src/NativeLog.cpp"
        "src/win/src/HttpFacesServerPath.cpp"
        "src/win/src/JsonLite.cpp"
    )
    target_include_directories(core_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
    )
    target_include_directories(core_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    )
    # Mock OpenCV stubs (cv::Rect etc.) for zero-dependency build
    target_include_directories(core_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/deps/opencv/include
    )
    target_compile_definitions(core_unit_tests PRIVATE RK_SKIP_OPENCV=1)
    target_link_libraries(core_unit_tests PRIVATE GTest::gtest GTest::gtest_main rk_core)
    if(MSVC)
        target_compile_options(core_unit_tests PRIVATE /utf-8)
    endif()
    add_test(NAME core_unit_tests COMMAND core_unit_tests)
endif()

# ── C++ 编译器契约（契约 #5 符号隐藏 + 契约 #4 异常禁止）──
# 仅对非测试的生产 target 应用 -fno-exceptions -fno-rtti -fvisibility=hidden
# 测试 target 需链接 GTest（依赖 RTTI），排除在应用范围外
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(RK_SAFETY_EXCLUDED
        core_unit_tests core_gtest_tests
        face_infer_unit_tests win_unit_tests ncnn_precision_test
        win_face_database_perf
    )
    foreach(t ${RK_ENGINE_TARGETS})
        if(NOT ${t} IN_LIST RK_SAFETY_EXCLUDED AND TARGET ${t})
            target_compile_options(${t} PRIVATE
                -fno-exceptions -fno-rtti -fvisibility=hidden)
        endif()
    endforeach()
endif()
