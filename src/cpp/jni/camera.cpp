/**
 * @file jni/camera.cpp
 * @brief JNI 相机与外部帧输入（camera domain）
 *
 * 管理外部帧的输入通道（YUV_420_888 直送和 byte array 两种方式）
 * 以及外部输入通道的配置。
 */
#include <jni.h>
#include <cstring>
#include <vector>

#include "Engine.h"
#include "NativeLog.h"
#include "JniMethodRegistry.h"
#include "jni_shared.h"

#define TAG "RK3288_JNI"

namespace {

// ========== 辅助函数：从 DirectByteBuffer 拷贝 ==========
bool copyDirectBytes(JNIEnv* env, jobject byteBuffer, std::size_t needBytes, std::vector<uint8_t>& out) {
    if (!byteBuffer) return false;
    void* addr = env->GetDirectBufferAddress(byteBuffer);
    const jlong cap = env->GetDirectBufferCapacity(byteBuffer);
    if (!addr || cap <= 0) return false;
    if (static_cast<std::size_t>(cap) < needBytes) return false;
    out.resize(needBytes);
    std::memcpy(out.data(), addr, needBytes);
    return true;
}

// ========== nativePushFrameYuv420888 ==========
jboolean camera_nativePushFrameYuv420888(
    JNIEnv* env, jobject /* this */,
    jobject yBuffer, jint yRowStride,
    jobject uBuffer, jint uRowStride, jint uPixelStride,
    jobject vBuffer, jint vRowStride, jint vPixelStride,
    jint width, jint height, jlong timestampNs,
    jint rotationDegrees, jboolean mirrored) {

    if (!g_engine) return JNI_FALSE;

    ExternalFrame f;
    f.format = ExternalFrameFormat::YUV_420_888;
    f.width = width;
    f.height = height;
    f.meta.timestampNs = static_cast<int64_t>(timestampNs);
    f.meta.rotationDegrees = rotationDegrees;
    f.meta.mirrored = (mirrored == JNI_TRUE);

    f.y.rowStride = yRowStride > 0 ? yRowStride : width;
    f.y.pixelStride = 1;
    f.u.rowStride = uRowStride;
    f.u.pixelStride = uPixelStride;
    f.v.rowStride = vRowStride;
    f.v.pixelStride = vPixelStride;

    if (width <= 0 || height <= 0 || f.y.rowStride < width) {
        rklog::logWarn(TAG, __func__, "nativePushFrameYuv420888 参数非法");
        return JNI_FALSE;
    }

    const int chromaW = (width + 1) / 2;
    const int chromaH = (height + 1) / 2;
    const std::size_t yNeed = static_cast<std::size_t>(f.y.rowStride) * static_cast<std::size_t>(height - 1)
                              + static_cast<std::size_t>(width);
    const std::size_t uNeed = (uRowStride > 0 && uPixelStride > 0)
                                  ? (static_cast<std::size_t>(uRowStride) * static_cast<std::size_t>(chromaH - 1)
                                     + static_cast<std::size_t>(chromaW - 1) * static_cast<std::size_t>(uPixelStride)
                                     + 1U)
                                  : 0U;
    const std::size_t vNeed = (vRowStride > 0 && vPixelStride > 0)
                                  ? (static_cast<std::size_t>(vRowStride) * static_cast<std::size_t>(chromaH - 1)
                                     + static_cast<std::size_t>(chromaW - 1) * static_cast<std::size_t>(vPixelStride)
                                     + 1U)
                                  : 0U;

    // 重要：GetDirectBufferAddress 指向 ByteBuffer 底层起始地址，不会自动应用 position/limit。
    // 约定：Java 侧传入前先 rewind()/position(0)，避免拷贝到错误偏移。
    if (!copyDirectBytes(env, yBuffer, yNeed, f.y.bytes)) {
        rklog::logWarn(TAG, __func__, "Y 平面需要 DirectByteBuffer 且容量足够");
        return JNI_FALSE;
    }
    if (uNeed > 0 && !copyDirectBytes(env, uBuffer, uNeed, f.u.bytes)) {
        rklog::logWarn(TAG, __func__, "U 平面需要 DirectByteBuffer 且容量足够");
        return JNI_FALSE;
    }
    if (vNeed > 0 && !copyDirectBytes(env, vBuffer, vNeed, f.v.bytes)) {
        rklog::logWarn(TAG, __func__, "V 平面需要 DirectByteBuffer 且容量足够");
        return JNI_FALSE;
    }

    return g_engine->pushExternalFrame(std::move(f)) ? JNI_TRUE : JNI_FALSE;
}

// ========== nativePushFrameYuv420888Bytes ==========
jboolean camera_nativePushFrameYuv420888Bytes(
    JNIEnv* env, jobject /* this */,
    jbyteArray yBytes, jint yRowStride,
    jbyteArray uBytes, jint uRowStride, jint uPixelStride,
    jbyteArray vBytes, jint vRowStride, jint vPixelStride,
    jint width, jint height, jlong timestampNs,
    jint rotationDegrees, jboolean mirrored) {

    if (!g_engine) return JNI_FALSE;
    if (!yBytes) return JNI_FALSE;

    ExternalFrame f;
    f.format = ExternalFrameFormat::YUV_420_888;
    f.width = width;
    f.height = height;
    f.meta.timestampNs = static_cast<int64_t>(timestampNs);
    f.meta.rotationDegrees = rotationDegrees;
    f.meta.mirrored = (mirrored == JNI_TRUE);

    f.y.rowStride = yRowStride > 0 ? yRowStride : width;
    f.y.pixelStride = 1;
    f.u.rowStride = uRowStride;
    f.u.pixelStride = uPixelStride;
    f.v.rowStride = vRowStride;
    f.v.pixelStride = vPixelStride;

    if (width <= 0 || height <= 0 || f.y.rowStride < width) {
        rklog::logWarn(TAG, __func__, "nativePushFrameYuv420888Bytes 参数非法");
        return JNI_FALSE;
    }

    const int chromaW = (width + 1) / 2;
    const int chromaH = (height + 1) / 2;
    const std::size_t yNeed = static_cast<std::size_t>(f.y.rowStride) * static_cast<std::size_t>(height - 1)
                              + static_cast<std::size_t>(width);
    const std::size_t uNeed = (uRowStride > 0 && uPixelStride > 0)
                                  ? (static_cast<std::size_t>(uRowStride) * static_cast<std::size_t>(chromaH - 1)
                                     + static_cast<std::size_t>(chromaW - 1) * static_cast<std::size_t>(uPixelStride)
                                     + 1U)
                                  : 0U;
    const std::size_t vNeed = (vRowStride > 0 && vPixelStride > 0)
                                  ? (static_cast<std::size_t>(vRowStride) * static_cast<std::size_t>(chromaH - 1)
                                     + static_cast<std::size_t>(chromaW - 1) * static_cast<std::size_t>(vPixelStride)
                                     + 1U)
                                  : 0U;

    if (static_cast<std::size_t>(env->GetArrayLength(yBytes)) < yNeed) return JNI_FALSE;
    f.y.bytes.resize(yNeed);
    env->GetByteArrayRegion(yBytes, 0, static_cast<jsize>(yNeed),
                            reinterpret_cast<jbyte*>(f.y.bytes.data()));

    if (uNeed > 0) {
        if (!uBytes) return JNI_FALSE;
        if (static_cast<std::size_t>(env->GetArrayLength(uBytes)) < uNeed) return JNI_FALSE;
        f.u.bytes.resize(uNeed);
        env->GetByteArrayRegion(uBytes, 0, static_cast<jsize>(uNeed),
                                reinterpret_cast<jbyte*>(f.u.bytes.data()));
    }

    if (vNeed > 0) {
        if (!vBytes) return JNI_FALSE;
        if (static_cast<std::size_t>(env->GetArrayLength(vBytes)) < vNeed) return JNI_FALSE;
        f.v.bytes.resize(vNeed);
        env->GetByteArrayRegion(vBytes, 0, static_cast<jsize>(vNeed),
                                reinterpret_cast<jbyte*>(f.v.bytes.data()));
    }

    return g_engine->pushExternalFrame(std::move(f)) ? JNI_TRUE : JNI_FALSE;
}

// ========== nativeConfigureExternalInput ==========
void camera_nativeConfigureExternalInput(
    JNIEnv* env, jobject /* this */,
    jboolean enabled, jint backpressureMode, jint queueCapacity) {

    RKLOG_ENTER(TAG);
    if (!g_engine) return;
    g_engine->configureExternalInput(static_cast<FrameBackpressureMode>(backpressureMode),
                                    queueCapacity > 0 ? static_cast<std::size_t>(queueCapacity) : 1);
    g_engine->setExternalInputEnabled(enabled == JNI_TRUE);
    rklog::logInfo(TAG, __func__,
                   std::string("nativeConfigureExternalInput enabled=") + (enabled == JNI_TRUE ? "1" : "0") +
                   " mode=" + std::to_string(backpressureMode) +
                   " cap=" + std::to_string(queueCapacity));
}

} // anonymous namespace

void registerCameraMethods(JniMethodRegistry& registry) {
    registry.add({
        {"nativePushFrameYuv420888",
         "(Ljava/nio/ByteBuffer;ILjava/nio/ByteBuffer;IILjava/nio/ByteBuffer;IIIIIJIZ)Z",
         reinterpret_cast<void*>(camera_nativePushFrameYuv420888)},
        {"nativePushFrameYuv420888Bytes",
         "([BI[BII[BIIIIIJIZ)Z",
         reinterpret_cast<void*>(camera_nativePushFrameYuv420888Bytes)},
        {"nativeConfigureExternalInput",
         "(ZII)V",
         reinterpret_cast<void*>(camera_nativeConfigureExternalInput)},
    });
}
