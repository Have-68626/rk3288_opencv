# ═══════════════════════════════════════════════════════════════════
# android.cmake — Android JNI native-lib target
# ═══════════════════════════════════════════════════════════════════

# ── 冲突标记门禁（tracked conflict-marker gate）──
# 未解析的 Git 冲突标记会阻断完整 Android native 构建（RK_SKIP_OPENCV=OFF），
# 且在 RK_SKIP_OPENCV=ON 时被 stub 变体遮蔽。此处在配置期拦截。
# 清单 = native-lib 全量构建所编的 **8 个 JNI 源**（与下方 `add_library(native-lib ...)` 的 JNI
# 源一致，含 src/cpp/src/JniMethodRegistry.cpp）：任一含冲突标记都会阻断该构建，故逐一同扫。
# add_library 另列的 4 个 `src/win/src/*.cpp` 与 JNI 无关（Windows/Android 共享），不属本门禁范围。
set(RK_JNI_GATE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/native-lib.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/JniMethodRegistry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/jni/camera.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/jni/config.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/jni/engine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/jni/native_face_bridge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/jni/preview.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/jni/registry.cpp"
)
foreach(_gate_file ${RK_JNI_GATE_FILES})
    file(READ "${_gate_file}" _gate_content)
    if(_gate_content MATCHES "(^|\n)<<<<<<< " OR _gate_content MATCHES "(^|\n)>>>>>>> ")
        message(FATAL_ERROR "Conflict markers found in ${_gate_file}. "
            "Resolve before Android native build.")
    endif()
endforeach()

find_library(log-lib log)
find_library(jnigraphics-lib jnigraphics)

if(NOT RK_SKIP_OPENCV)
    # Android JNI Shared Library
    add_library(native-lib SHARED
        ${RK_CORE_LITE_SOURCES} ${RK_FACE_INFER_CORE_SOURCES} ${RK_ADAPTER_SOURCES} ${RK_PIPELINE_SOURCES}
        "src/cpp/native-lib.cpp"
        "src/cpp/src/JniMethodRegistry.cpp"
        "src/cpp/jni/camera.cpp"
        "src/cpp/jni/engine.cpp"
        "src/cpp/jni/preview.cpp"
        "src/cpp/jni/config.cpp"
        "src/cpp/jni/native_face_bridge.cpp"
        "src/cpp/jni/registry.cpp"
        "src/win/src/FaceDetector.cpp"
        "src/win/src/LbphEmbedder.cpp"
        "src/win/src/DnnSsdFaceDetector.cpp"
        "src/win/src/JsonLite.cpp"
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
