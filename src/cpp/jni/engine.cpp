/**
 * @file jni/engine.cpp
 * @brief JNI 引擎生命周期管理（engine domain）
 *
 * 包括 Engine 初始化、启动/停止、模式切换、翻转控制和测试接口。
 */
#include <jni.h>
#include <thread>
#include <memory>
#include <cstdlib>
#include <android/log.h>

#include "Engine.h"
#include "NativeLog.h"
#include "JniMethodRegistry.h"
#include "jni_shared.h"

#define TAG "RK3288_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

// ========== stringFromJNI（简单测试接口） ==========
jstring engine_stringFromJNI(JNIEnv* env, jobject /* this */) {
    RKLOG_ENTER(TAG);
    std::string hello = "Hello from RK3288 C++ Engine";
    return env->NewStringUTF(hello.c_str());
}

// ========== nativeInitFile ==========
jboolean engine_nativeInitFile(
    JNIEnv* env, jobject thiz,
    jstring filePath, jstring cascadePath, jstring storagePath) {

    RKLOG_ENTER(TAG);

    stopAndJoinEngineThreadIfRunning();

    // Update global activity ref（受 g_activityMutex 保护）
    {
        std::lock_guard<std::mutex> lock(g_activityMutex);
        if (g_activity) env->DeleteGlobalRef(g_activity);
        g_activity = env->NewGlobalRef(thiz);
    }

    const char* path = filePath ? env->GetStringUTFChars(filePath, nullptr) : nullptr;
    const char* cascade = cascadePath ? env->GetStringUTFChars(cascadePath, nullptr) : nullptr;
    const char* storage = storagePath ? env->GetStringUTFChars(storagePath, nullptr) : nullptr;

    std::string pathStr = path ? path : "";
    std::string cascadeStr = cascade ? cascade : "";
    std::string storageStr = storage ? storage : "";

    if (path) env->ReleaseStringUTFChars(filePath, path);
    if (cascade) env->ReleaseStringUTFChars(cascadePath, cascade);
    if (storage) env->ReleaseStringUTFChars(storagePath, storage);

    LOGI("Initializing Engine with Mock File: %s...", pathStr.c_str());
    if (!g_engine) {
        g_engine = std::make_unique<rk_core::Engine>();
    }

    g_cancelInit.store(false);
    g_engine->clearCancelInit();
    g_engine->setOnResultCallback(sendRecognitionResult);

    try {
        return g_engine->initialize(pathStr, cascadeStr, storageStr);
    } catch (const std::exception& e) {
        LOGE("Engine::initialize threw std::exception: %s", e.what());
        return JNI_FALSE;
    } catch (...) {
        LOGE("Engine::initialize threw unknown exception");
        return JNI_FALSE;
    }
}

// ========== nativeInit ==========
jboolean engine_nativeInit(
    JNIEnv* env, jobject thiz,
    jstring cameraId, jstring cascadePath, jstring storagePath) {

    RKLOG_ENTER(TAG);

    stopAndJoinEngineThreadIfRunning();

    // Update global activity ref（受 g_activityMutex 保护）
    {
        std::lock_guard<std::mutex> lock(g_activityMutex);
        if (g_activity) env->DeleteGlobalRef(g_activity);
        g_activity = env->NewGlobalRef(thiz);
    }

    const char* cascade = cascadePath ? env->GetStringUTFChars(cascadePath, nullptr) : nullptr;
    if (cascadePath && env->ExceptionCheck()) {
        env->ExceptionClear();
        return JNI_FALSE;
    }
    const char* storage = storagePath ? env->GetStringUTFChars(storagePath, nullptr) : nullptr;
    if (storagePath && env->ExceptionCheck()) {
        if (cascade) env->ReleaseStringUTFChars(cascadePath, cascade);
        env->ExceptionClear();
        return JNI_FALSE;
    }

    std::string cascadeStr = cascade ? cascade : "";
    std::string storageStr = storage ? storage : "";

    if (cascade) env->ReleaseStringUTFChars(cascadePath, cascade);
    if (storage) env->ReleaseStringUTFChars(storagePath, storage);

    // Java 层 cameraId 已改为 String
    //   null          → 外部帧输入（camIdInt = -1）
    //   纯数字         → 相机索引（camIdInt = 数值）
    //   文件路径       → 调用 initialize(string filePath) 重载
    //   其他非法格式   → 返回 false
    bool useFilePathInit = false;
    std::string camIdStr;
    int camIdInt = -1;
    if (cameraId) {
        const char* camStr = env->GetStringUTFChars(cameraId, nullptr);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return JNI_FALSE;
        }
        if (camStr) {
            camIdStr = camStr;
            env->ReleaseStringUTFChars(cameraId, camStr);
        }
        if (!camIdStr.empty()) {
            bool isFilePath = camIdStr.find('.') != std::string::npos ||
                              camIdStr.find('/') != std::string::npos ||
                              camIdStr.find('\\') != std::string::npos;
            if (isFilePath) {
                useFilePathInit = true;
            } else {
                char* end = nullptr;
                long val = std::strtol(camIdStr.c_str(), &end, 10);
                if (end == camIdStr.c_str() || *end != '\0' || val < 0) {
                    LOGE("Invalid cameraId format: %s", camIdStr.c_str());
                    return JNI_FALSE;
                }
                camIdInt = static_cast<int>(val);
            }
        }
    }

    if (!g_engine) {
        g_engine = std::make_unique<rk_core::Engine>();
    }

    g_cancelInit.store(false);
    g_engine->clearCancelInit();
    g_engine->setOnResultCallback(sendRecognitionResult);

    if (useFilePathInit) {
        LOGI("Initializing Engine with file path: %s...", camIdStr.c_str());
        try {
            return g_engine->initialize(camIdStr, cascadeStr, storageStr);
        } catch (const std::exception& e) {
            LOGE("Engine::initialize(filePath) threw: %s", e.what());
            return JNI_FALSE;
        }
    }

    LOGI("Initializing Engine with Camera ID: %d...", camIdInt);
    try {
        return g_engine->initialize(camIdInt, cascadeStr, storageStr);
    } catch (const std::exception& e) {
        LOGE("Engine::initialize(cameraId) threw: %s", e.what());
        return JNI_FALSE;
    } catch (...) {
        LOGE("Engine::initialize threw unknown exception");
        return JNI_FALSE;
    }
}

// ========== nativeStart ==========
void engine_nativeStart(JNIEnv* /* env */, jobject /* this */) {
    RKLOG_ENTER(TAG);
    LOGI("Starting Engine...");
    if (!g_engine) return;
    std::lock_guard<std::mutex> lock(g_engineThreadMutex);
    if (g_engineThread.joinable()) {
        LOGI("Engine thread already running, ignore nativeStart");
        return;
    }
    g_engineThread = std::thread([]() { g_engine->run(); });
}

// ========== nativeStop ==========
void engine_nativeStop(JNIEnv* /* env */, jobject /* this */) {
    RKLOG_ENTER(TAG);
    LOGI("Stopping Engine...");
    stopAndJoinEngineThreadIfRunning();
}

// ========== nativeRequestCancelInit ==========
void engine_nativeRequestCancelInit(JNIEnv* /* env */, jobject /* this */) {
    g_cancelInit.store(true);
    if (g_engine) {
        g_engine->requestCancelInit();
    }
}

// ========== nativeSetMode ==========
void engine_nativeSetMode(JNIEnv* /* env */, jobject /* this */, jint mode) {
    RKLOG_ENTER(TAG);
    if (g_engine) {
        g_engine->setMode(static_cast<MonitoringMode>(mode));
    }
}

// ========== nativeSetFlip ==========
void engine_nativeSetFlip(JNIEnv* /* env */, jobject /* this */,
                          jboolean flipX, jboolean flipY) {
    if (!g_engine) return;
    g_engine->setFlip(flipX == JNI_TRUE, flipY == JNI_TRUE);
}

} // anonymous namespace

void registerEngineMethods(JniMethodRegistry& registry) {
    registry.add({
        {"stringFromJNI", "()Ljava/lang/String;",
         reinterpret_cast<void*>(engine_stringFromJNI)},
        {"nativeInitFile", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z",
         reinterpret_cast<void*>(engine_nativeInitFile)},
        {"nativeInit", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z",
         reinterpret_cast<void*>(engine_nativeInit)},
        {"nativeStart", "()V",
         reinterpret_cast<void*>(engine_nativeStart)},
        {"nativeStop", "()V",
         reinterpret_cast<void*>(engine_nativeStop)},
        {"nativeRequestCancelInit", "()V",
         reinterpret_cast<void*>(engine_nativeRequestCancelInit)},
        {"nativeSetMode", "(I)V",
         reinterpret_cast<void*>(engine_nativeSetMode)},
        {"nativeSetFlip", "(ZZ)V",
         reinterpret_cast<void*>(engine_nativeSetFlip)},
    });
}
