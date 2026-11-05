# ═══════════════════════════════════════════════════════════════════
# linux.cmake — Linux 运行时服务 target（阶段 4a–4d）
# ═══════════════════════════════════════════════════════════════════
# 仅在非 Android 的 POSIX（Linux）且未跳过 OpenCV 时被包含。
# 依赖决策：D2 OpenSSL libcrypto、D6 libcurl、D7 V4L2(CameraSource)、D8 前台 + SIGTERM。
# ncnn/MPP 在 Linux 服务中默认关闭（RK_HAVE_NCNN=0 / RK_HAVE_MPP=0），与 Windows 构建互不影响。

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

set(RK_LINUX_PROFILE_SOURCES
    "src/win/src/ProfileWinRecognizer.cpp"
    "src/cpp/src/FaceRuntimeProfile.cpp"
    "src/cpp/src/FaceAlign.cpp"
    "src/cpp/src/pipeline/RealtimeFacePipeline.cpp"
    "src/cpp/src/pipeline/ScrfdDecoder.cpp"
    "src/cpp/src/pipeline/FacePostProcessor.cpp"
    "src/cpp/src/adapters/ScrfdNcnnAdapter.cpp"
    "src/cpp/src/adapters/ArcFaceAdapter.cpp")

add_executable(linux_local_service
    "src/win/app/win_local_service_main.cpp"
    "src/win/src/Platform.cpp"
    "src/win/src/PlatformNet.cpp"
    "src/win/src/CameraSource.cpp"
    "src/win/src/WinConfig.cpp"
    "src/win/src/JsonLite.cpp"
    "src/win/src/WinCrypto.cpp"
    "src/win/src/WinJsonConfig.cpp"
    "src/win/src/JsonSchemaValidator.cpp"
    "src/win/src/StructuredLogger.cpp"
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
    ${RK_LINUX_PROFILE_SOURCES}
)

target_include_directories(linux_local_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include/adapters
)
# 不要把 src/win/include/rk_win 加进来：ScrfdNcnnAdapter.h 的 "FaceDetector.h" 必须解析到
# src/cpp/include/FaceDetector.h（rk_core 接口），而不是 rk_win/FaceDetector.h（Windows 实现）。

target_compile_definitions(linux_local_service PRIVATE RK_HAVE_NCNN=0 RK_HAVE_MPP=0)

target_link_libraries(linux_local_service
    ${RK_OPENCV_FULL_LIBS}
    OpenSSL::Crypto
    CURL::libcurl
    Threads::Threads
)

add_custom_command(TARGET linux_local_service POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/src/win/app/webroot
            $<TARGET_FILE_DIR:linux_local_service>/webroot
    COMMENT "Copy webroot for Linux HTTP static hosting"
)

# 服务冒烟：启动 → /api/v1/health = 200 → SIGTERM 优雅退出。端口被占用时 SKIP（77）。
enable_testing()
add_test(NAME linux_service_smoke
    COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=
            python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/linux_service_smoke.py
            $<TARGET_FILE:linux_local_service>)
set_tests_properties(linux_service_smoke PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    TIMEOUT 60
    SKIP_RETURN_CODE 77)
