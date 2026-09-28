# ═══════════════════════════════════════════════════════════════════
# windows.cmake — Windows 平台 target 定义
# ═══════════════════════════════════════════════════════════════════
# 注意：仅在 WIN32 AND NOT RK_SKIP_OPENCV AND RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION 时被包含

if(RK_BUILD_WINDOWS_LOCAL_WEB_SERVICE)
    add_executable(win_local_service
        "src/win/app/win_local_service_main.cpp"
        "src/win/src/WinConfig.cpp"
        "src/win/src/JsonLite.cpp"
        "src/win/src/WinCrypto.cpp"
        "src/win/src/WinJsonConfig.cpp"
        "src/win/src/JsonSchemaValidator.cpp"
        "src/win/src/StructuredLogger.cpp"
        "src/win/src/MfCamera.cpp"
        "src/win/src/FaceDetector.cpp"
        "src/win/src/LbphEmbedder.cpp"
        "src/win/src/FaceDatabase.cpp"
        "src/win/src/FaceRecognizer.cpp"
        "src/win/src/ArcFaceWinRecognizer.cpp"
        "src/cpp/src/ArcFaceEmbedder.cpp"
        "src/win/src/DnnSsdFaceDetector.cpp"
        "src/win/src/OverlayRenderer.cpp"
        "src/win/src/FacesJson.cpp"
        "src/win/src/HttpFacesServer.cpp"
        "src/win/src/StreamSessionRunner.cpp"
        "src/win/src/StreamSession.cpp"
        "src/win/src/HttpFacesServerPath.cpp"
        "src/win/src/HttpFacesPoster.cpp"
        "src/win/src/FramePipeline.cpp"
        "src/win/src/FrameProcessor.cpp"
        "src/win/src/SideEffectSink.cpp"
        "src/win/src/RuntimeBootstrap.cpp"
        "src/win/src/EndpointRegistry.cpp"
        "src/win/src/EventLogger.cpp"
        "src/win/src/JsonEndpointHandlers.cpp"
        "src/cpp/src/FileHash.cpp"
        "src/cpp/src/NativeLog.cpp"
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
        ${RK_OPENCV_FULL_LIBS}
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

    # models/ 为 gitignore 的二进制目录（`models/*` 仅跟踪 .gitkeep 与 android/ 下的文本清单），
    # 开发机上可能不存在 ⇒ 必须先判存在再拷，否则 copy_directory 会让构建失败（同 CMakeLists.txt
    # 里 yolo_face_int8.param 的守卫风格）。
    # ⚠️ 已知后果（如实登记，本批不改）：`if(EXISTS ...)` 是**配置期**（configure 时）求值 ⇒ 若 configure
    #    时刻 models/ 不存在，本 POST_BUILD 步骤**永不会被注册**；此后即使跑量化脚本生成了 models/，
    #    再构建也**不会**拷贝——必须先**重新 configure**（重新生成 CMake 缓存）才会注册该步骤。
    #    这是沿用 CMakeLists.txt:260 同一个 `if(EXISTS ...)` 配置期守卫的取舍：改用构建期判据需要把
    #    逻辑挪进脚本或生成器表达式，收益不抵复杂度，故两处保持一致。
    # ⚠️ 收益边界（如实）：本项**不会**让默认 ArcFace 权重变可用——`models/` 内并没有
    #    `arcface_w600k_r50.onnx`（默认路径在 WinJsonConfig.cpp 指向它）；DNN 默认路径则在
    #    `<exeDir>/storage/models/`，也不受影响。本项只把 ncnn 权重 / 校准图 / 清单搬到 exe 同级，
    #    便利开发机上直接跑；部署机走 install()、本就不含这些目录。
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models")
        add_custom_command(TARGET win_local_service POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/models
                    $<TARGET_FILE_DIR:win_local_service>/models
            COMMENT "Copy models dir (best-effort) for Windows local service"
        )
    endif()
endif()

if(RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION_UI)
    add_executable(win_camera_face_recognition WIN32
        "src/win/app/win_camera_face_recognition_main.cpp"
        "src/win/src/WinConfig.cpp"
        "src/win/src/JsonLite.cpp"
        "src/win/src/WinCrypto.cpp"
        "src/win/src/WinJsonConfig.cpp"
        "src/win/src/JsonSchemaValidator.cpp"
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
        "src/win/src/StreamSessionRunner.cpp"
        "src/win/src/StreamSession.cpp"
        "src/win/src/HttpFacesServerPath.cpp"
        "src/win/src/HttpFacesPoster.cpp"
        "src/win/src/AbcTestRunner.cpp"
        "src/win/src/FramePipeline.cpp"
        "src/win/src/FrameProcessor.cpp"
        "src/win/src/SideEffectSink.cpp"
        "src/win/src/RuntimeBootstrap.cpp"
        "src/win/src/EndpointRegistry.cpp"
        "src/win/src/DisplaySettings.cpp"
        "src/win/src/D3D11Renderer.cpp"
        "src/win/src/RenderMetricsLogger.cpp"
        "src/win/src/EventLogger.cpp"
        "src/win/src/JsonEndpointHandlers.cpp"
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
        ${RK_OPENCV_FULL_LIBS}
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

    # models/ 拷贝：与上方 win_local_service 的同名块保持一致（完整理由与该处的配置期守卫、
    # 收益边界说明见其注释，此处不再复述以免两处漂移）。
    # 简言之：`models/*` 为 gitignore 的二进制目录，开发机上可能不存在 ⇒ 必须先判存在再拷，
    # 否则 copy_directory 会让构建失败；`if(EXISTS ...)` 是配置期求值，若 configure 时该目录不存在
    # 则本 POST_BUILD 步骤不会被注册，需重新 configure 才会生效。
    # ⚠️ 收益边界（如实）：本项**不会**让默认 ArcFace 权重变可用——`models/` 内并没有
    #    `arcface_w600k_r50.onnx`（默认路径在 WinJsonConfig.cpp 指向它）。
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models")
        add_custom_command(TARGET win_camera_face_recognition POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/models
                    $<TARGET_FILE_DIR:win_camera_face_recognition>/models
            COMMENT "Copy models dir (best-effort) for win_camera_face_recognition UI"
        )
    endif()
endif()

# ── Windows CLI 工具 ──

add_executable(win_face_eval_cli
    "src/win/tools/win_face_eval_cli.cpp"
    "src/win/src/WinConfig.cpp"
    "src/win/src/JsonLite.cpp"
    "src/win/src/WinCrypto.cpp"
    "src/win/src/WinJsonConfig.cpp"
    "src/win/src/JsonSchemaValidator.cpp"
    "src/win/src/FaceDetector.cpp"
    "src/win/src/LbphEmbedder.cpp"
    "src/win/src/FaceDatabase.cpp"
    "src/win/src/FaceRecognizer.cpp"
    "src/win/src/StructuredLogger.cpp"
    # FaceRecognizer.cpp 调用 rk_wcfr::calculateSHA256（定义在 src/cpp/src/FileHash.cpp 内）
    # ⇒ 本 target 必须一并编译该 TU，否则链接期 LNK2019 未解析外部符号。
    # 此前这两个 CLI 长期缺这一行且 CI 不构建它们，故缺口一直未被发现。
    "src/cpp/src/FileHash.cpp"
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
    ${RK_OPENCV_FULL_LIBS}
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
    "src/win/src/JsonSchemaValidator.cpp"
    "src/win/src/FaceDetector.cpp"
    "src/win/src/LbphEmbedder.cpp"
    "src/win/src/FaceDatabase.cpp"
    "src/win/src/FaceRecognizer.cpp"
    # FaceRecognizer.cpp 调用 rk_wcfr::calculateSHA256（定义在 src/cpp/src/FileHash.cpp 内）
    # ⇒ 本 target 必须一并编译该 TU，否则链接期 LNK2019 未解析外部符号。
    "src/cpp/src/FileHash.cpp"
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
    ${RK_OPENCV_FULL_LIBS}
    psapi
    bcrypt
    crypt32
)

# ── 视频 → 检测序列 导出器（任务 C1 离线回放载体的最后一环）──
# 依赖 OpenCV videoio 解码；序列化器 (pipeline::ReplayScriptWriter) 零 OpenCV 依赖，
# 与 tests/cpp/test_track_replay.cpp 的解析器共用同一文本格式。
add_executable(win_replay_export_cli
    "src/win/tools/win_replay_export_cli.cpp"
    "src/cpp/src/pipeline/ReplayScriptWriter.cpp"
    "src/cpp/src/FileHash.cpp"
    "src/win/src/WinConfig.cpp"
    "src/win/src/JsonLite.cpp"
    "src/win/src/WinCrypto.cpp"
    "src/win/src/WinJsonConfig.cpp"
    "src/win/src/JsonSchemaValidator.cpp"
    "src/win/src/FaceDetector.cpp"
    "src/win/src/LbphEmbedder.cpp"
    "src/win/src/FaceDatabase.cpp"
    "src/win/src/FaceRecognizer.cpp"
    "src/win/src/StructuredLogger.cpp"
)
target_include_directories(win_replay_export_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
)
target_compile_definitions(win_replay_export_cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(win_replay_export_cli PRIVATE /utf-8)
endif()
target_link_libraries(win_replay_export_cli
    ${RK_OPENCV_FULL_LIBS}
    shlwapi
    shell32
    bcrypt
    crypt32
)

# ── Windows 单元测试 ──

enable_testing()
add_executable(win_unit_tests
    "tests/win/win_unit_tests_main.cpp"
    "tests/win/test_face_metrics.cpp"
    "tests/win/test_lbph_embedder.cpp"
    "tests/win/test_http_faces_server.cpp"
    "tests/win/test_runtime_bootstrap.cpp"
    # FACE-ARCWIN-002 自检判别用例（2026-09-26）：需要定位 tests/fixtures/onnx/ 下的合成 ONNX 固定件，
    # 故本 target 必须定义 RK_TEST_SOURCE_ROOT（见下方 target_compile_definitions）。
    "tests/win/test_arcface_win_recognizer_selfcheck.cpp"
    "tests/win/test_event_logger.cpp"
    "tests/win/test_log_rotation.cpp"
    "tests/win/test_endpoint_registry.cpp"
    "tests/win/test_frame_pipeline_reload.cpp"
    "tests/win/test_win_json_config.cpp"
    "tests/win/test_face_database_encryption.cpp"
    "src/win/src/RuntimeBootstrap.cpp"
    "src/win/src/FramePipeline.cpp"
    "src/win/src/MfCamera.cpp"
    "src/win/src/EndpointRegistry.cpp"
    "src/win/src/HttpFacesServer.cpp"
    "src/win/src/StreamSessionRunner.cpp"
    "src/win/src/StreamSession.cpp"
    "src/win/src/JsonEndpointHandlers.cpp"
    "src/win/src/FacesJson.cpp"
    "src/cpp/src/FileHash.cpp"
    "src/win/src/HttpFacesServerPath.cpp"
    "src/win/src/LbphEmbedder.cpp"
    "src/win/src/FaceRecognizer.cpp"
    "src/win/src/FaceDatabase.cpp"
    "src/win/src/FaceDetector.cpp"
    "src/win/src/WinConfig.cpp"
    "src/win/src/JsonLite.cpp"
    "src/win/src/WinCrypto.cpp"
    "src/win/src/WinJsonConfig.cpp"
    "src/win/src/JsonSchemaValidator.cpp"
    "src/win/src/FrameProcessor.cpp"
    "src/win/src/SideEffectSink.cpp"
    "src/win/src/ArcFaceWinRecognizer.cpp"
    "src/cpp/src/ArcFaceEmbedder.cpp"
    "src/cpp/src/NativeLog.cpp"
    "src/win/src/DnnSsdFaceDetector.cpp"
    "src/win/src/StructuredLogger.cpp"
    # test_event_logger.cpp 要真正链接 EventLogger 的实现（轮转逻辑在 .cpp 内）。此前 win_unit_tests
    # 只编了 StructuredLogger.cpp —— 缺这一行会 LNK2019（EventLogger 的符号未解析）。
    "src/win/src/EventLogger.cpp"
    # test_log_rotation.cpp 的 C 组要真正链接 RenderMetricsLogger 的实现（轮转逻辑在 .cpp 内）。
    # 缺这一行会 LNK2019（RenderMetricsLogger::open / append 的符号未解析）。
    # ⚠️ 该 .cpp 另需 psapi（GetProcessMemoryInfo —— 本 target 已链接）与 **dxgi**
    #    （CreateDXGIFactory1，用于 gpuMemoryBytesBestEffort）——dxgi 已加入下方 target_link_libraries，
    #    详见该处的实测更正说明。
    "src/win/src/RenderMetricsLogger.cpp"
)
target_include_directories(win_unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
)
target_compile_definitions(win_unit_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
# 结构/固定件类用例需定位源码根以读取仓内文件（`add_test` 未设 WORKING_DIRECTORY ⇒ ctest 的 cwd 是
# 构建目录，不能靠相对路径）。与 cmake/core.cmake 对 core_unit_tests 的同一宏保持同一口径。
# ⚠️ **副作用（实测，2026-09-26）**：一旦定义，`tests/win/test_http_faces_server.cpp` 里
#    `#ifdef RK_TEST_SOURCE_ROOT` 包着的结构守卫会**从休眠变为启用**（win_unit_tests 的 GTest 用例数
#    46 → 47）。
# ★ **事实订正（2026-09-28，S4 变异实测推翻本注释原话）**：原文写「删掉这一行会让那条守卫**静默消失**
#    （既不注册、也不 skip、也不报错）」——**已不成立**。实测：删掉本行 + 重新 configure，后果是
#    **硬编译失败** —— `tests/win/test_arcface_win_recognizer_selfcheck.cpp` 里那对
#    `#ifndef RK_TEST_SOURCE_ROOT / #error`（就在该文件开头）会让构建直接报 **C1189**，
#    压根到不了「用例数 47→46」那一步。⇒ 删除本行**不再**是静默降覆盖，而是**响亮的构建中断**。
#    ⚠️ 但原文的担忧方向仍然有效、且这正是要一起看的地方：**若将来有人删掉那个 `#error`**，
#    本行一旦缺席就会真的退回「守卫静默消失」。⇒ 改动前请**同时**看本行与该自检用例的 `#error`。
# ⚠️ 本行**必须恰好一份**：本批曾因编辑失误把它写成**两行**（2026-09-28，被增量核查代理 A5 查出）。
#    两行并存时，注释里那条判据（「删掉本行 ⇒ C1189」）**当场失效** —— 删掉多出来的那份后另一份仍在，
#    宏照样有定义，构建照过，于是真会退回「守卫静默消失（47→46）」，即本注释正要否定的形态。
#    功能上 CMake 会在生成期去重（每目标的 PreprocessorDefinitions 只出现一次），故**无行为影响**；
#    但注释的可核性会被破坏 ⇒ 改这段时请 `grep -c` 数一遍。
target_compile_definitions(win_unit_tests PRIVATE RK_TEST_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(win_unit_tests PRIVATE /utf-8)
endif()
target_link_libraries(win_unit_tests
    ${RK_OPENCV_FULL_LIBS}
    mfplat
    mfreadwrite
    mfuuid
    mf
    ole32
    bcrypt
    crypt32
    ws2_32
    # RenderMetricsLogger.cpp 的 gpuMemoryBytesBestEffort 调 CreateDXGIFactory1（见该 .cpp 的
    # 该函数）；psapi 侧（GetProcessMemoryInfo）由既有链接面提供。缺 dxgi 会 LNK2019 + LNK1120。
    # ⚠️ 实测更正：曾据「Release 下 /OPT:REF 会丢弃未被引用的 gpuMemoryBytesBestEffort COMDAT」
    #   断言此处**无需** dxgi —— **该断言经真实构建证伪**（Release + VS2022 下确实报
    #   `无法解析的外部符号 CreateDXGIFactory1`）。该函数是非 inline 的外部符号，不构成本可被
    #   /OPT:REF 剥离的 COMDAT，故无论 Release / Debug 都必须显式链接。
    dxgi
)
add_test(NAME win_unit_tests COMMAND win_unit_tests)
# 超时保护：win_unit_tests 内的多线程 / 真实 socket 用例若阻塞，会**无限挂起** ctest，
# 故设 120s 把「挂起」降级为一次可观测的 Timeout 失败。
# ★ 归因订正（2026-09-28，证伪代理查实、本人复核）：原注释把成因写作「（B06-014）」——
#   **B06-014 实为「FramePipeline 运行时所有权并发的 UAF」（模型热重载释放处理线程正在使用的
#   对象，见 .remember/today-2026-08-06.done.md 与提交 a30b93b），与本处挂起无关**。
#   本超时实际是模板式复制（同一策略也用于 face_infer_unit_tests，见 CMakeLists.txt），
#   并非为某一编号任务而设。⇒ 勿再由该注释推断此处防的是 UAF。
# ★ 如实登记（同一轮，**限定条件必须一并引用**）：本仓曾观测到一次
#   `3 - win_unit_tests (Timeout)`（整体 exit 8，由上一轮审计代理报告），但
#   **当时的 Testing/Temporary/LastTest.log 已被后续跑覆盖 ⇒ 该次挂起的位置与成因均无存证**；
#   且 gtest 输出经 ctest 管道重定向后为块缓冲，被超时杀掉时末段会丢失
#   ⇒ 连「卡在哪个用例」都不能断言。故该次事件**既未证实、也未被否定**。
#   ⚠️ **尤其不得据此宣称「负载性时序 flake 已被排除」**（增量核查 2026-09-28 实测）：同一二进制、
#     同一机器、同一构建下，安静的 ctest 记 `Passed 51.11s`，而本轮**其他几次** ctest 只记
#     11–14s；负载较高时直接跑同一 exe 的**进程总时长**更达 **73.3 / 101.6 / 118.5s**
#     （那几次只跑 1 条 gtest，工作量**更少**）。⇒ **负载敏感度至少 2–4×，最坏样本距本行这个 120s
#     只剩 ≈1.5s**。因此「负载」是一个**仍有实测支持的**候选解释，**不要**引用本注释去否定它。
#   ⚠️ 本行数值**偏紧**（是否上调属需拍板的调整，本轮**不动**）；本轮真正查实并已修的是
#     **测试装置的一处自锁**（`test_http_faces_server.cpp`：先关客户端 socket 再 join +
#     补 `SO_SNDTIMEO` 与服务端生产口径对齐），并补了一条**带看门狗的判别用例**覆盖该路径。
set_tests_properties(win_unit_tests PROPERTIES TIMEOUT 120)

add_executable(win_face_database_perf
    "tests/win/test_face_database_perf.cpp"
    "src/win/src/FaceDatabase.cpp"
    "src/win/src/WinCrypto.cpp"
)
target_include_directories(win_face_database_perf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
)
target_compile_definitions(win_face_database_perf PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(win_face_database_perf PRIVATE /utf-8)
endif()
target_link_libraries(win_face_database_perf
    ${RK_OPENCV_FULL_LIBS}
    bcrypt
    crypt32
)
add_test(NAME win_face_database_perf COMMAND win_face_database_perf)

# ── Install targets ──
# Only install main binaries and the shared library, not tests or benchmarks
if(TARGET win_local_service)
    install(TARGETS win_local_service RUNTIME DESTINATION bin)
endif()
if(TARGET win_camera_face_recognition)
    install(TARGETS win_camera_face_recognition RUNTIME DESTINATION bin)
endif()
if(TARGET rk3288_cli)
    install(TARGETS rk3288_cli RUNTIME DESTINATION bin)
endif()
if(TARGET native-lib)
    install(TARGETS native-lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
endif()
if(TARGET inference_bench_cli)
    install(TARGETS inference_bench_cli RUNTIME DESTINATION bin)
endif()
if(TARGET win_face_eval_cli)
    install(TARGETS win_face_eval_cli RUNTIME DESTINATION bin)
endif()
if(TARGET win_face_bench_cli)
    install(TARGETS win_face_bench_cli RUNTIME DESTINATION bin)
endif()
if(TARGET win_replay_export_cli)
    install(TARGETS win_replay_export_cli RUNTIME DESTINATION bin)
endif()
