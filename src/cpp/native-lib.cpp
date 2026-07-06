/**
 * @file native-lib.cpp
 * @brief JNI 入口（精简版）
 *
 * 仅保留全局变量定义、共享辅助函数、JNI_OnLoad/JNI_OnUnload。
 * JNI 函数实现已按领域拆分到 src/cpp/jni/ 目录。
 */

#include <jni.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <string>
#include <android/native_window.h>

#include "Engine.h"

// ========== 共享全局变量（外部链接，供 jni/*.cpp 引用） ==========
std::unique_ptr<rk_core::Engine> g_engine;
std::thread g_engineThread;
std::mutex g_engineThreadMutex;
JavaVM* g_vm = nullptr;
jobject g_activity = nullptr;
std::mutex g_activityMutex;
std::mutex g_previewMutex;
ANativeWindow* g_previewWindow = nullptr;
std::atomic<uint64_t> g_previewGeneration{0};
std::atomic<bool> g_cancelInit{false};

// ========== 共享辅助函数 ==========

void stopAndJoinEngineThreadIfRunning() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(g_engineThreadMutex);
        if (!g_engineThread.joinable()) {
            return;
        }
        if (g_engine) {
            g_engine->stop();
        }
        t = std::move(g_engineThread);
    }
    if (t.joinable()) {
        t.join();
    }
    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        if (g_previewWindow) {
            ANativeWindow_release(g_previewWindow);
            g_previewWindow = nullptr;
        }
    }
}

void sendRecognitionResult(const std::string& result) {
    jobject activityLocal;
    {
        std::lock_guard<std::mutex> lock(g_activityMutex);
        activityLocal = g_activity;
    }
    if (!g_vm || !activityLocal) return;

    JNIEnv* env;
    int getEnvStat = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    bool attached = false;

    if (getEnvStat == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != 0) {
            return;
        }
        attached = true;
    }

    jclass cls = env->GetObjectClass(activityLocal);
    jmethodID mid = env->GetMethodID(cls, "onNativeResult", "(Ljava/lang/String;)V");
    if (mid) {
        jstring jStr = env->NewStringUTF(result.c_str());
        env->CallVoidMethod(activityLocal, mid, jStr);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(jStr);
    }
    env->DeleteLocalRef(cls);

    if (attached) {
        g_vm->DetachCurrentThread();
    }
}

// ========== JNI 入口/出口 ==========

// registerAllNativeMethods 由 jni/registry.cpp 实现
jint registerAllNativeMethods(JNIEnv* env);

extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void* /* reserved */) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    const jint registered = registerAllNativeMethods(env);
    if (registered < 0) return JNI_ERR;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
JNI_OnUnload(JavaVM* /* vm */, void* /* reserved */) {
    stopAndJoinEngineThreadIfRunning();
    std::lock_guard<std::mutex> lock(g_previewMutex);
    if (g_previewWindow) {
        ANativeWindow_release(g_previewWindow);
        g_previewWindow = nullptr;
    }
}
