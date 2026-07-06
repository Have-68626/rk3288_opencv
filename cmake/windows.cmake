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
        "src/win/src/CameraSession.cpp"
        "src/win/src/RuntimeBootstrap.cpp"
        "src/win/src/EndpointRegistry.cpp"
        "src/win/src/EventLogger.cpp"
        "src/win/src/EndpointRegistry.cpp"
        "src/win/src/JsonEndpointHandlers.cpp"
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
        "src/win/src/CameraSession.cpp"
        "src/win/src/RuntimeBootstrap.cpp"
        "src/win/src/EndpointRegistry.cpp"
        "src/win/src/DisplaySettings.cpp"
        "src/win/src/D3D11Renderer.cpp"
        "src/win/src/RenderMetricsLogger.cpp"
        "src/win/src/EventLogger.cpp"
        "src/win/src/EndpointRegistry.cpp"
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

# ── Windows 单元测试 ──

enable_testing()
add_executable(win_unit_tests
    "tests/win/win_unit_tests_main.cpp"
    "tests/win/test_face_metrics.cpp"
    "tests/win/test_lbph_embedder.cpp"
    "tests/win/test_http_faces_server.cpp"
    "tests/win/test_runtime_bootstrap.cpp"
    "tests/win/test_endpoint_registry.cpp"
    "src/win/src/RuntimeBootstrap.cpp"
    "tests/win/test_endpoint_registry.cpp"
    "src/win/src/EndpointRegistry.cpp"
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
    ${RK_OPENCV_FULL_LIBS}
    bcrypt
    crypt32
)
add_test(NAME win_unit_tests COMMAND win_unit_tests)

add_executable(win_face_database_perf
    "tests/win/test_face_database_perf.cpp"
    "src/win/src/FaceDatabase.cpp"
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
