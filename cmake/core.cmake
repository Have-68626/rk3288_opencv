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
        "tests/cpp/test_log_rate_limiter.cpp"
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
    # §10.8 测试接缝宏 RK_ENABLE_TRACK_RESET_SEAM：让 TrackCoordinator::reset() 仅在测试侧可见
    # （见 src/cpp/include/pipeline/TrackCoordinator.h:86）。本宏**不改变 ABI**——与 RK_ENABLE_TEST_SEAMS
    # 不同：后者令 Engine 的析构由非虚变虚、多出 vptr（属 ABI 变更），白名单被严格限定为
    # core_gtest_tests；本宏只增删一个成员函数声明，不改变对象布局，故不复用后者。
    # reset() 的测试调用点**全部**在 core_unit_tests（tests/cpp/test_track_coordinator.cpp、
    # tests/cpp/test_track_replay.cpp），故只在此定义。使用边界由下方
    # rk_assert_track_reset_seam_boundary() 双向钉死。
    target_compile_definitions(core_unit_tests PRIVATE RK_ENABLE_TRACK_RESET_SEAM)
    # §10.11 D4b 测试侧判别力（FACE-TEST-002）：本 target 同时编译 RealtimeFacePipeline.cpp 与
    # test_realtime_face_pipeline.cpp。全局选项留空时，.cpp 的平台默认回落 __aarch64__（x64 上为
    # false），恰与头文件默认(恒 false)相同 ⇒ 「构造函数是否真的写入了默认值」在 x64/CI 上不可判别。
    # 故仅对本**测试** target 显式注入 RK_BENCH_ENFORCE_BUDGET=1，使 kDefaultEnforceBudget 与头文件
    # 默认相反，从而对**构造写入**产生判别力（删掉构造函数那行赋值即令用例翻红）。生产 target 不受影响。
    # 全局选项非空时由其经 rk_core INTERFACE 统一注入，此处不再定义（避免宏重定义）。
    # ⚠️ 代价（如实登记，勿当作已封死）：注入使 `#if defined(RK_BENCH_ENFORCE_BUDGET)` 在本 target
    #    **恒成立** ⇒ .cpp 的 `#elif defined(__aarch64__)` / `#else` 两个**平台回落分支在此不被编译**，
    #    即「非 arm64 平台默认 = false」这条语义在 core_unit_tests 内**无任何断言**。该覆盖由
    #    core_gtest_tests 的 RealtimeFacePipelinePlatformDefault 用例承担（该 target 不定义此宏，
    #    真走 #else 分支，见 tests/cpp/test_engine.cpp）。故本注入**只**服务于构造写入判别力。
    if(RK_BENCH_ENFORCE_BUDGET STREQUAL "")
        target_compile_definitions(core_unit_tests PRIVATE RK_BENCH_ENFORCE_BUDGET=1)
    endif()
    if(MSVC)
        target_compile_options(core_unit_tests PRIVATE /utf-8)
    endif()
    # ⚠️ **必须给 WORKING_DIRECTORY**（2026-09-26 二次审计实测）：不设时 ctest 的 cwd = 构建目录，
    #    而本 target 里 `NcnnParamGuard.AcceptsRealRepoParamsIfPresent` 用**相对路径**扫
    #    `models/` 与 `deps/insightface/`（**目录扫描 + `GTEST_SKIP`**，不是 fileExists 判定）
    #    ⇒ 构建目录下它**静默跳过**（实测读数：ctest **163 过/40 跳** vs 仓库根直接跑 exe **164/39**，
    #     两边都 exit 0，差别正好是这一条；验收代理用逐例复合键独立复核过）
    #    ⇒ 照 §10 那条 ctest 命令跑会**少覆盖一条用例且看不出**。
    #    设源码根后，本机 ctest 与「从仓库根直接跑 exe」口径一致（读 ctest 须带 `-C Release`：
    #    多配置生成器下裸 `ctest` 会报 6 条 Not Run）。与 face_infer_unit_tests / core_gtest_tests 同口径。
    #    ⚠️ **CI 行为不变的条件（不得漏写）**：windows job 的 ctest 步（ci.yml 的 `Run ctest`）位于
    #    量化/INT8 步**之前**，且 `models/*.param` **不入库**（`git ls-files "*.param"` 为空）
    #    ⇒ 全新检出时该用例照旧跳过。**一旦将来在 ctest 之前产出 `models/*.param`，这一条会由
    #    SKIP 变 RUN（覆盖是收益，但读数会变）。**
    add_test(NAME core_unit_tests COMMAND core_unit_tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

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
set(RK_TEST_SEAM_TARGETS core_gtest_tests win_unit_tests)
set(RK_TEST_SEAM_SCANNED_INTERFACE_LIBS rk_core)
# ★ 2026-09-29（项 B）：白名单新增 win_unit_tests。该宏现在**有两个消费者**——
#   · core_gtest_tests：Engine 的 protected virtual 接缝（onBeforeRenderPublish）＋虚析构（ABI 变更）；
#   · win_unit_tests：WinJsonConfigStore 的 setReloadTestHookForTesting / setRotateTestHookForTesting
#     （编译期开关，门后无该 setter 与成员 ⇒ 生产二进制里不存在，误用变编译错误；**不改任何 ABI**）。
#   WinJsonConfigStore 的接缝唯一调用点在 tests/win/test_win_json_config.cpp（属 win_unit_tests），
#   故该宏只在 win_unit_tests 定义（见 cmake/windows.cmake）。两个 target 各自独立编译 WinJsonConfig.cpp /
#   Engine.cpp，宏状态在各自 target 内一致 ⇒ 无 ODR/布局问题；生产 target 不定义本宏 ⇒ 接缝不参与编译。
# ⚠️ 复用同一宏（非自创）的代价（如实登记）：本断言的方向②「whitelist 必须携带」对 win_unit_tests 的
#   实际拦截力较弱——宏缺失时 WinJsonConfigStore 的接缝 setter **不存在** ⇒ 调用它的测试**编译失败**
#   （响亮），并非静默失效；方向② 在此同样只是**冗余防线**（同 core_gtest_tests 处的如实说明）。
function(rk_assert_test_seam_boundary)
    # 「断言被跳过」必须与「断言通过」在日志里**长得不一样** —— 否则本断言自身的空转无人察觉
    # （子代理 4 S3：原实现下二者都是静默的）。仅当白名单 target 在本配置下**全部**不存在时提示。
    # ⚠️ 必须逐项判存在（不可写成 `if(NOT TARGET ${RK_TEST_SEAM_TARGETS})`）——该变量现在是**列表**
    #    （core_gtest_tests;win_unit_tests），`TARGET` 操作符只接受一个参数，列表展开会触发 CMake 配置期报错。
    set(_rk_seam_any_present FALSE)
    foreach(_rk_seam_t ${RK_TEST_SEAM_TARGETS})
        if(TARGET ${_rk_seam_t})
            set(_rk_seam_any_present TRUE)
        endif()
    endforeach()
    if(NOT _rk_seam_any_present)
        message(STATUS
            "rk_assert_test_seam_boundary: 白名单 target(s) ${RK_TEST_SEAM_TARGETS} 在本配置下均不存在"
            "(RK_SKIP_OPENCV / Android / 非 Windows) ⇒ 方向② 不适用；方向① 仍照常执行")
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

# ── 配置期断言（§10.8）：测试接缝宏 RK_ENABLE_TRACK_RESET_SEAM 的使用边界 ──
# 该宏把 `TrackCoordinator::reset()` 从「不存在」变为「存在且可调用」
# （src/cpp/include/pipeline/TrackCoordinator.h:86）。它**不改变任何 target 的 ABI**——与
# RK_ENABLE_TEST_SEAMS 有本质区别：后者让 `Engine` 析构由非虚变虚、多出 vptr（ABI 变更），
# 其白名单因此被 rk_assert_test_seam_boundary 严格限定为 core_gtest_tests。故本宏**不复用**后者：
#   · 复用会把一个「改 ABI、且白名单受限」的宏借给 core_unit_tests，既不必要又危险；
#   · reset() 的测试调用点全在 core_unit_tests，本宏只需出现在它一个 target 上。
# 后果全在编译期：宏缺失 ⇒ reset() 静默不存在 ⇒ 调用它的测试**编译失败**（响亮，非运行期静默）。
# 仍双向钉死：① 除白名单外任何 target 都**不得携带**；② 白名单内的 target **必须携带**。
#
# 扫描集合显式带上 core_gtest_tests（它不在 RK_ENGINE_TARGETS）与 INTERFACE 库 rk_core（与孪生断言
# rk_assert_test_seam_boundary 的 RK_TEST_SEAM_SCANNED_INTERFACE_LIBS 对齐）：若将来有人给它们或
# 任何非白名单 target 加上该宏，配置期即 FATAL，不留运行期隐患。只扫 target 自身的定义读不到 INTERFACE
# 传播值，故这里必须把接口库单列进扫描集合，否则「把宏挂到 rk_core」这条向量会完全静默。
set(RK_TRACK_RESET_SEAM_TARGETS core_unit_tests)
set(RK_TRACK_RESET_SEAM_SCANNED_INTERFACE_LIBS rk_core)
function(rk_assert_track_reset_seam_boundary)
    # 「断言被跳过」必须与「断言通过」在日志里**长得不一样**（同 rk_assert_test_seam_boundary）。
    # 仅当白名单 target 在本配置下不存在时提示（如 RK_SKIP_OPENCV / Android 下 core_unit_tests 缺失）。
    if(NOT TARGET ${RK_TRACK_RESET_SEAM_TARGETS})
        message(STATUS
            "rk_assert_track_reset_seam_boundary: 白名单 target ${RK_TRACK_RESET_SEAM_TARGETS} 在本配置下不存在"
            "⇒ 方向② 不适用；方向① 仍照常执行")
    endif()
    foreach(t ${RK_ENGINE_TARGETS} ${RK_TRACK_RESET_SEAM_TARGETS} core_gtest_tests ${RK_TRACK_RESET_SEAM_SCANNED_INTERFACE_LIBS})
        if(NOT TARGET ${t})
            continue()
        endif()
        # 同时看「自身定义」与「自身对外传播的定义」，任一含该宏都算该 target 携带它
        #（未设置时 get_target_property 返回 <var>-NOTFOUND，if() 判假，归一化后可靠——
        # 写法照抄 rk_assert_test_seam_boundary:301-305）。
        get_target_property(_rk_tr_defs ${t} COMPILE_DEFINITIONS)
        get_target_property(_rk_tr_iface ${t} INTERFACE_COMPILE_DEFINITIONS)
        if(NOT _rk_tr_defs)
            set(_rk_tr_defs "")
        endif()
        if(NOT _rk_tr_iface)
            set(_rk_tr_iface "")
        endif()
        set(_rk_tr_seam_defs "${_rk_tr_defs};${_rk_tr_iface}")

        if(_rk_tr_seam_defs MATCHES "RK_ENABLE_TRACK_RESET_SEAM")
            if(NOT ${t} IN_LIST RK_TRACK_RESET_SEAM_TARGETS)
                message(FATAL_ERROR
                    "CONTRACT_ASSERT_FAIL: target ${t} 携带 RK_ENABLE_TRACK_RESET_SEAM "
                    "(唯一允许携带它的 target 是: ${RK_TRACK_RESET_SEAM_TARGETS})")
            endif()
        elseif(${t} IN_LIST RK_TRACK_RESET_SEAM_TARGETS)
            message(FATAL_ERROR
                "CONTRACT_ASSERT_FAIL: 测试 target ${t} 未携带 RK_ENABLE_TRACK_RESET_SEAM "
                "(宏缺失会让 TrackCoordinator::reset() 静默不存在 ⇒ 调用它的测试编译失败或接缝静默失效)")
        endif()
    endforeach()
endfunction()

# ── 配置期断言（§10.11 D4b）：基准预算覆盖宏 RK_BENCH_ENFORCE_BUDGET 的统一性 ──
# 该宏覆盖 RealtimeBenchmarkConfig.enforceBudget 的平台默认值（见 RealtimeFacePipeline.cpp 的
# kDefaultEnforceBudget）。风险不在单 target 内部（同一 .cpp 只编译一次），而在「显式选项只施加到
# 部分编译该文件的 target」：test_realtime_face_pipeline.cpp 依据宏的有无/取值推断期望行为，
# 一旦它与编译 RealtimeFacePipeline.cpp 的 target 对宏的可见性不一致，断言与实现就会错位。
# 故把「凡编译 RealtimeFacePipeline.cpp 的 target 都必须携带该宏」钉死。
# 留空（未显式设置）⇒ 不注入、按 __aarch64__ 回落，无统一性可查；此时「跳过」须与「通过」长得不一样。
set(RK_BENCH_BUDGET_SCANNED_INTERFACE_LIBS rk_core)
function(rk_assert_bench_budget_uniformity)
    if(RK_BENCH_ENFORCE_BUDGET STREQUAL "")
        message(STATUS
            "rk_assert_bench_budget_uniformity: RK_BENCH_ENFORCE_BUDGET 未显式设置"
            "⇒ 预算按 __aarch64__ 回落，跳过一致性断言")
        return()
    endif()
    foreach(t ${RK_ENGINE_TARGETS} core_gtest_tests)
        if(NOT TARGET ${t})
            continue()
        endif()
        get_target_property(_rk_bb_srcs ${t} SOURCES)
        if(NOT _rk_bb_srcs OR NOT _rk_bb_srcs MATCHES "RealtimeFacePipeline\\.cpp")
            continue()
        endif()
        # 有效定义 = 自身 COMPILE_DEFINITIONS ∪ 自身 INTERFACE_COMPILE_DEFINITIONS
        #（归一化写法照抄 rk_assert_test_seam_boundary:293-301）。
        get_target_property(_rk_bb_defs ${t} COMPILE_DEFINITIONS)
        get_target_property(_rk_bb_iface ${t} INTERFACE_COMPILE_DEFINITIONS)
        if(NOT _rk_bb_defs)
            set(_rk_bb_defs "")
        endif()
        if(NOT _rk_bb_iface)
            set(_rk_bb_iface "")
        endif()
        set(_rk_bb_all "${_rk_bb_defs};${_rk_bb_iface}")
        # 也接受「经 INTERFACE 库 rk_core 传播」的形态（core_unit_tests 正是以此方式携带）：
        # 宏挂在 rk_core 的 INTERFACE_COMPILE_DEFINITIONS 上，只有链接它的 target 生效，而
        # get_target_property(<消费者> COMPILE_DEFINITIONS) 读不到它（同接缝断言记录的 M1 教训）。
        if(NOT _rk_bb_all MATCHES "RK_BENCH_ENFORCE_BUDGET")
            get_target_property(_rk_bb_libs ${t} LINK_LIBRARIES)
            foreach(_rk_lib ${RK_BENCH_BUDGET_SCANNED_INTERFACE_LIBS})
                if(NOT TARGET ${_rk_lib})
                    continue()
                endif()
                if(_rk_bb_libs AND ${_rk_lib} IN_LIST _rk_bb_libs)
                    get_target_property(_rk_lib_defs ${_rk_lib} INTERFACE_COMPILE_DEFINITIONS)
                    if(_rk_lib_defs MATCHES "RK_BENCH_ENFORCE_BUDGET")
                        set(_rk_bb_all "${_rk_bb_all};${_rk_lib_defs}")
                    endif()
                endif()
            endforeach()
        endif()
        if(NOT _rk_bb_all MATCHES "RK_BENCH_ENFORCE_BUDGET")
            message(FATAL_ERROR
                "CONTRACT_ASSERT_FAIL: target ${t} 编译 RealtimeFacePipeline.cpp "
                "却未携带 RK_BENCH_ENFORCE_BUDGET"
                "（显式选项未被统一施加 ⇒ 同一进程内预算语义分歧）")
        endif()
    endforeach()
endfunction()
