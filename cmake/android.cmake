# ═══════════════════════════════════════════════════════════════════
# android.cmake — Android JNI native-lib target
# ═══════════════════════════════════════════════════════════════════

find_library(log-lib log)
find_library(jnigraphics-lib jnigraphics)

if(NOT RK_SKIP_OPENCV)
    # Android JNI Shared Library
    add_library(native-lib SHARED
        ${RK_CORE_LITE_SOURCES} ${RK_FACE_INFER_CORE_SOURCES} ${RK_ADAPTER_SOURCES}
        "src/cpp/native-lib.cpp"
        "src/cpp/src/JniMethodRegistry.cpp"
        "src/cpp/jni/camera.cpp"
        "src/cpp/jni/engine.cpp"
        "src/cpp/jni/preview.cpp"
        "src/cpp/jni/config.cpp"
        "src/cpp/jni/registry.cpp"
        "src/win/src/FaceDetector.cpp"
        "src/win/src/LbphEmbedder.cpp"
        "src/win/src/DnnSsdFaceDetector.cpp"
    )
    # Link Libraries for JNI
    target_link_libraries(
        native-lib
        ${RK_OPENCV_FULL_LIBS}
    )
else()
    add_library(native-lib SHARED "src/cpp/native-lib-stub.cpp")
    # 模拟 OpenCV 头文件以便在 RK_SKIP_OPENCV=ON 且不编译源文件的情况下提供依赖
    target_include_directories(native-lib PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/deps/opencv/include
    )
    target_compile_definitions(native-lib PRIVATE RK_SKIP_OPENCV=1)
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include/adapters
    ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include
)

target_compile_definitions(native-lib PRIVATE RK_HAVE_NCNN=$<IF:$<BOOL:${RK_HAVE_NCNN}>,1,0>)
target_compile_definitions(native-lib PRIVATE RK_HAVE_LIBYUV=$<IF:$<BOOL:${RK_HAVE_LIBYUV}>,1,0>)
if(RK_HAVE_LIBYUV AND libyuv_SOURCE_DIR)
    target_include_directories(native-lib PRIVATE ${libyuv_SOURCE_DIR}/include)
endif()
if(RK_HAVE_NCNN AND RK_NCNN_TARGET)
    target_link_libraries(native-lib ${RK_NCNN_TARGET})
endif()
if(RK_HAVE_MPP)
    target_include_directories(native-lib PRIVATE ${RK_MPP_INCLUDE_DIR})
    target_link_libraries(native-lib mpp)
endif()
