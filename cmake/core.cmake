# ═══════════════════════════════════════════════════════════════════
# core.cmake — rk_core INTERFACE 库 + CORE_SOURCES 变量
# ═══════════════════════════════════════════════════════════════════

# ── googletest 版本（唯一事实源）──────────────
# core.cmake 主分支与 CMakeLists.txt 兜底分支共用 GIT_TAG，消除双写漂移（RW8 ⑤）
set(RK_GTEST_GIT_TAG "release-1.12.1")

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
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/pipeline/FacePostProcessor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/pipeline/ScrfdDecoder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/pipeline/ResultPublisher.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/pipeline/PerfReporter.cpp
# 注意：pipeline/ReplayScriptWriter.{h,cpp} **刻意不列入本表**。它在本仓库的唯一消费者是
# 「视频 → 检测序列」导出 CLI（cmake/windows.cmake 显式列源）与 core_unit_tests
#（下方测试源列表显式列源）。列入本表会把它无谓地拉进 rk3288_cli / core_gtest_tests /
# Android native-lib 等并不引用它的 target，平白多出编译单元（A2 审计 L-2）。
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
    # 受控 GTest：禁用系统预编译 GTest（Anaconda 等用动态 CRT，与项目静态 /MT 不匹配，
    # 链接报 LNK2038 RuntimeLibrary 不匹配 / LNK2005 重复符号），强制 FetchContent
    # 源码构建 googletest 并继承项目 CRT（MSVC 下 gtest_force_shared_crt OFF → 静态 /MT）
    set(CMAKE_DISABLE_FIND_PACKAGE_GTest ON CACHE BOOL "" FORCE)
    if(MSVC)
        # gtest_force_shared_crt 仅影响 MSVC CRT（OFF → 静态 /MT）；Linux 下属噪音（B06-005），不设 FORCE
        set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG ${RK_GTEST_GIT_TAG}
    )
    FetchContent_MakeAvailable(googletest)
    add_executable(core_unit_tests
        "tests/cpp/core_unit_tests_main.cpp"
        "tests/cpp/test_frame_input_channel.cpp"
        "tests/cpp/test_face_search.cpp"
        "tests/cpp/test_threshold_policy.cpp"
        "tests/cpp/test_event_manager.cpp"
        "tests/cpp/test_file_hash.cpp"
        "tests/cpp/test_connection_quota.cpp"
        "tests/cpp/test_acceleration_contract.cpp"
        "tests/cpp/test_ncnn_param_guard.cpp"
        "tests/cpp/test_native_log_redaction.cpp"
        "tests/cpp/test_track_coordinator.cpp"
        "tests/cpp/test_track_replay.cpp"
        "tests/cpp/test_replay_script_writer.cpp"
        "tests/cpp/test_face_runtime_profile_id.cpp"
        "tests/cpp/test_face_post_processor.cpp"
        "tests/cpp/test_scrfd_decoder.cpp"
        "tests/cpp/test_realtime_face_pipeline.cpp"
        "tests/cpp/test_checked_arith.cpp"
        "src/cpp/src/pipeline/TrackCoordinator.cpp"
        "src/cpp/src/pipeline/FacePostProcessor.cpp"
        "src/cpp/src/pipeline/ScrfdDecoder.cpp"
        "src/cpp/src/pipeline/RealtimeFacePipeline.cpp"
        "src/cpp/src/pipeline/ReplayScriptWriter.cpp"
        "src/cpp/src/FaceRuntimeProfile.cpp"
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
    # S-SEC-B09-003：test_checked_arith.cpp 结构测试需定位源码根以读取接入点源文件
    target_compile_definitions(core_unit_tests PRIVATE RK_TEST_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(core_unit_tests PRIVATE GTest::gtest GTest::gtest_main rk_core)
    if(MSVC)
        target_compile_options(core_unit_tests PRIVATE /utf-8)
    endif()
    add_test(NAME core_unit_tests COMMAND core_unit_tests)

    # Q-CQ-B06-003 编译器契约测试：异常禁用 flag 与语法一致性回归保护
    # （Clang/GNU -fno-exceptions 下必须编译失败；MSVC /EH- 下必须触发 C4530）
    # WORK_DIR 传入构建树目录，避免写死源码树 build/（B06-003）
    # 探针注册条件显式化：真实 MSVC / Clang / GNU / IntelLLVM 都注册契约探针；
    # IntelLLVM 注册但走 Clang 分支（不传 RK_CONTRACT_MSVC，规避其 /EHs- 硬错误）
    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC|Clang|GNU|IntelLLVM")
        # clang-cl 检测（RW8 ③）：clang-cl 下 MSVC=TRUE 但 CMAKE_CXX_COMPILER_ID=Clang。
        # 其 /EH- 对 throw 是硬错误（非 MSVC 的 C4530 警告），契约探针应走 Clang/GNU
        # 分支（-fno-exceptions）；故仅对真实 MSVC（ID=MSVC）传 RK_CONTRACT_MSVC=1
        set(_rk_contract_msvc_args)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            set(_rk_contract_msvc_args "-DRK_CONTRACT_MSVC=1")
        endif()
        add_test(NAME compiler_contract_exceptions
            COMMAND ${CMAKE_COMMAND}
            "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            ${_rk_contract_msvc_args}
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/contract_test_build"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/contract_test.cmake)
    endif()
endif()

# ── 编译器契约辅助函数 ──
# 调用点见 CMakeLists.txt 末尾（所有 target 定义之后）——函数已接线，非"未接线"。
# 不能在 include 时内联执行：此时生产 target 尚未定义。
# 契约分组定义在文件作用域（而非函数内）：rk_apply_compiler_contract（应用 flag）与
# rk_assert_contract_flags（配置期断言）两个函数共用同一分组，避免函数作用域变量
# 跨函数不可见导致分组漂移（RW8 ①）。
set(RK_SAFETY_EXCLUDED
    core_unit_tests core_gtest_tests
    face_infer_unit_tests win_unit_tests ncnn_precision_test
    win_face_database_perf
)
# 需异常/RTTI 的目标：自身用 try/catch，或依赖 OpenCV 头（flann throw/typeid）
# P3 ②：win_* 目标（win_local_service/win_camera_face_recognition 等）自身含 try/catch 源，
# 在 clang-cl（-fno-exceptions）下会编译失败；但 clang-cl 未在 CI/项目使用（Windows 均 MSVC），
# 故不将其列入本列表，仅记录风险——未来启用 clang-cl 时需评估（RW8 低危遗留）。
set(RK_EXCEPTIONS_REQUIRED
    native-lib rk3288_cli opencv_verify_cli inference_bench_cli
)

function(rk_apply_compiler_contract)
    # 门控用 CMAKE_CXX_COMPILER_ID 替代裸 MSVC 变量：IntelLLVM/clang-cl 下 MSVC=TRUE
    # 但 ID≠MSVC。clang-cl（ID=Clang）命中下方 Clang|GNU 分支；IntelLLVM（ID=IntelLLVM）
    # 两分支均不命中 → 无操作：不应用任何契约 flag（-fno-exceptions/-fvisibility=hidden），
    # 配置期断言 rk_assert_contract_flags 同样跳过。当前项目不构建 IntelLLVM，无操作安全；
    # 且避免了误走 /EHsc 分支（IntelLLVM 的 /EHs- 是硬错误）。（RW8 低危遗留，P3 ①）
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # 项目代码使用 C++ 异常（try/catch）与 OpenCV 头，target 级启用 MSVC 异常处理
        # 避免 C4530（使用异常处理程序但未启用展开语义）；target 级不传播到 OpenCV 子目录
        foreach(t ${RK_ENGINE_TARGETS})
            if(TARGET ${t})
                target_compile_options(${t} PRIVATE /EHsc)
            endif()
        endforeach()
        # core_gtest_tests 不在 RK_ENGINE_TARGETS，但编译 DnnSsdFaceDetector.cpp 等 try/catch 源
        if(TARGET core_gtest_tests)
            target_compile_options(core_gtest_tests PRIVATE /EHsc)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        foreach(t ${RK_ENGINE_TARGETS})
            if(NOT ${t} IN_LIST RK_SAFETY_EXCLUDED AND TARGET ${t})
                if(${t} IN_LIST RK_EXCEPTIONS_REQUIRED)
                    # 使用异常语法（try/catch）或依赖 OpenCV 头（flann 用 throw/typeid）：
                    # 保留异常与 RTTI，仅隐藏符号
                    target_compile_options(${t} PRIVATE -fvisibility=hidden)
                else()
                    target_compile_options(${t} PRIVATE
                        -fno-exceptions -fno-rtti -fvisibility=hidden)
                endif()
            endif()
        endforeach()
    endif()
endfunction()

# 配置期断言（B06-003 / RW8 ①）：configure 末尾、rk_apply_compiler_contract 调用后单独
# 执行，读回 COMPILE_OPTIONS 验证契约 flag 已落到 target。从契约函数中抽出，避免与
# target_compile_options 同函数紧邻形成"自证式"断言，并防 target 定义顺序漂移。
function(rk_assert_contract_flags)
    # 与 rk_apply_compiler_contract 同源门控：仅真实 MSVC（ID=MSVC）断言 /EHsc
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        foreach(t ${RK_ENGINE_TARGETS})
            if(TARGET ${t})
                get_target_property(_rk_opts ${t} COMPILE_OPTIONS)
                if(NOT _rk_opts MATCHES "/EHsc")
                    message(FATAL_ERROR "CONTRACT_ASSERT_FAIL: ${t} 未应用 /EHsc")
                endif()
            endif()
        endforeach()
        # core_gtest_tests 不在 RK_ENGINE_TARGETS，但 rk_apply_compiler_contract 对其单独
        # 应用 /EHsc；断言一并覆盖，防止该 flag 后续被移除而漏报（RW8 ③）
        if(TARGET core_gtest_tests)
            get_target_property(_rk_opts core_gtest_tests COMPILE_OPTIONS)
            if(NOT _rk_opts MATCHES "/EHsc")
                message(FATAL_ERROR "CONTRACT_ASSERT_FAIL: core_gtest_tests 未应用 /EHsc")
            endif()
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        foreach(t ${RK_ENGINE_TARGETS})
            if(NOT ${t} IN_LIST RK_SAFETY_EXCLUDED AND TARGET ${t})
                if(${t} IN_LIST RK_EXCEPTIONS_REQUIRED)
                    # 需异常分支不含 -fno-exceptions，以 -fvisibility=hidden 为单锚
                    get_target_property(_rk_opts ${t} COMPILE_OPTIONS)
                    if(NOT _rk_opts MATCHES "-fvisibility=hidden")
                        message(FATAL_ERROR "CONTRACT_ASSERT_FAIL: ${t} 未应用 -fvisibility=hidden")
                    endif()
                else()
                    # 双锚：-fno-exceptions 与 -fvisibility=hidden 都必须存在，
                    # 防止仅删 -fno-exceptions 不被单锚断言捕获（RW8 ③）
                    # P3 ③：本双锚 else 分支当前无 CI 覆盖——Linux/Android 非排除目标
                    # 全为 RK_EXCEPTIONS_REQUIRED（只走上方单锚），win_* 目标仅 MSVC 构建，
                    # clang-cl/IntelLLVM 未启用。故 -fno-rtti 亦未纳入锚定（保持现状，
                    # 避免给无覆盖分支增加无法验证的断言）。
                    get_target_property(_rk_opts ${t} COMPILE_OPTIONS)
                    if(NOT _rk_opts MATCHES "-fno-exceptions")
                        message(FATAL_ERROR "CONTRACT_ASSERT_FAIL: ${t} 未应用 -fno-exceptions")
                    endif()
                    if(NOT _rk_opts MATCHES "-fvisibility=hidden")
                        message(FATAL_ERROR "CONTRACT_ASSERT_FAIL: ${t} 未应用 -fvisibility=hidden")
                    endif()
                endif()
            endif()
        endforeach()
    endif()
endfunction()

# ── 配置期断言（FACE-CLEAN-019）：测试接缝宏 RK_ENABLE_TEST_SEAMS 的使用边界 ──
# 该宏让 Engine 的析构由非虚变虚（多出 vptr）并允许 test_engine.cpp 的 BarrierEngine 覆写
# onBeforeRenderPublish。它**只允许出现在测试 target 上**——一旦漏进生产 target，后果全在
# 运行期（ABI / 性能差异），**编译期完全无声**。故在此把两个方向都钉死：
#   ① 除白名单外，任何 target 都**不得携带**它；
#   ② 白名单内的 target **必须携带**它。
#      ⚠️ **理由须如实（子代理 4 S3 指出原措辞夸大）**：本仓里「拿掉该宏」**并不是**静默失效 ——
#      `Engine.h` 的 `onBeforeRenderPublish` 整个声明都写在 `#if defined(RK_ENABLE_TEST_SEAMS)` 内，
#      而 `test_engine.cpp` 的 `BarrierEngine` **无条件**声明该类并使用了 `override`
#      ⇒ 宏一旦缺失就是**编译错误（响亮）**。故方向 ② 实为一条**冗余防线**（无害且便宜，保留），
#      它防的是「将来有人去掉 `override`、或把虚函数挪到宏外」这类**形态变化**，
#      **不是**当前主要风险的拦截者。原措辞把危险夸大了，已更正。
#
# 生效范围（**如实，勿高估**）：
#   · `core_gtest_tests` 定义在 `if(NOT ANDROID)` ∧ `if(NOT RK_SKIP_OPENCV)` 内 ⇒
#     **`RK_SKIP_OPENCV=ON` 与 Android 配置下它根本不是 target，方向 ② 完全不执行（函数空转）**。
#     真正生效的是 Windows / 全量配置；CI 的 windows job 恰好构建 `core_gtest_tests`，故该路径有覆盖
#     （A2 审计 L2）。
#   · 方向 ① 的覆盖面见下方「扫描集合」处的局限说明。
#
# 扫描集合 = 引擎 target + 白名单 + **会被大量 target 链接的共享 INTERFACE 库**。
# ⚠️ 必须含 INTERFACE 库：宏若挂在 INTERFACE 库上会**传播**给所有链接它的 target，而
#    `get_target_property(<消费者> COMPILE_DEFINITIONS)` **看不到**它（返回 `-NOTFOUND`）——
#    只查目标自身定义会**静默漏报**（A2 审计 M1，已用探针复现）。本仓唯一的共享 INTERFACE 库
#    是 `rk_core`（本文件顶部 `add_library(rk_core INTERFACE)`）。
# ⚠️ **仍存的局限（如实记录，勿当作已封死）**：任意第三方/未来引入的 INTERFACE 库的传递传播、
#    以及目录级 `add_compile_definitions()`，均**不在**本断言的覆盖内。要彻底封死须改读实际
#    编译命令行，代价与脆弱性都更高，故本轮不做。
set(RK_TEST_SEAM_TARGETS core_gtest_tests)
set(RK_TEST_SEAM_SCANNED_INTERFACE_LIBS rk_core)
function(rk_assert_test_seam_boundary)
    # 「断言被跳过」必须与「断言通过」在日志里**长得不一样** —— 否则本断言自身的空转无人察觉
    # （子代理 4 S3：原实现下二者都是静默的）。仅当白名单 target 在本配置下不存在时提示。
    if(NOT TARGET ${RK_TEST_SEAM_TARGETS})
        message(STATUS
            "rk_assert_test_seam_boundary: 白名单 target ${RK_TEST_SEAM_TARGETS} 在本配置下不存在"
            "(RK_SKIP_OPENCV / Android) ⇒ 方向② 不适用；方向① 仍照常执行")
    endif()
    foreach(t ${RK_ENGINE_TARGETS} ${RK_TEST_SEAM_TARGETS} ${RK_TEST_SEAM_SCANNED_INTERFACE_LIBS})
        if(NOT TARGET ${t})
            continue()
        endif()
        # 同时看「自身定义」与「自身对外传播的定义」：任一含该宏都算该 target 携带它。
        # （`get_target_property` 未设置时返回 `<var>-NOTFOUND`，而 CMake 的 `if(<var>)` 把
        #   「以 -NOTFOUND 结尾」判为假，故下面的归一化写法可靠。）
        get_target_property(_rk_defs ${t} COMPILE_DEFINITIONS)
        get_target_property(_rk_iface_defs ${t} INTERFACE_COMPILE_DEFINITIONS)
        if(NOT _rk_defs)
            set(_rk_defs "")
        endif()
        if(NOT _rk_iface_defs)
            set(_rk_iface_defs "")
        endif()
        set(_rk_seam_defs "${_rk_defs};${_rk_iface_defs}")

        if(_rk_seam_defs MATCHES "RK_ENABLE_TEST_SEAMS")
            if(NOT ${t} IN_LIST RK_TEST_SEAM_TARGETS)
                message(FATAL_ERROR
                    "CONTRACT_ASSERT_FAIL: target ${t} 携带 RK_ENABLE_TEST_SEAMS "
                    "(该宏会改变 Engine 的 ABI；唯一允许携带它的 target 是: ${RK_TEST_SEAM_TARGETS})")
            endif()
        elseif(${t} IN_LIST RK_TEST_SEAM_TARGETS)
            message(FATAL_ERROR
                "CONTRACT_ASSERT_FAIL: 测试 target ${t} 未携带 RK_ENABLE_TEST_SEAMS "
                "(测试接缝会静默失效：BarrierEngine 将覆写一个非虚函数且不报错)")
        endif()
    endforeach()
endfunction()
