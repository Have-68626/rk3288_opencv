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
#include "jni_shared.h" // registerAllNativeMethods 等共享声明（定义在 jni/registry.cpp）

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

namespace {

// onNativeResult 的 jclass/jmethodID 缓存：避免**逐帧** GetMethodID（需在类方法表中查找，
// 且失败时会污染挂起的异常状态）。g_activity 可能被 Java 侧替换为新 Activity 实例，故缓存**必须
// 能失效**——每次仍取一次 localCls = GetObjectClass(activityLocal)，用 IsSameObject 判定是否换了
// 类，不同则释放旧全局引用、重建（NewGlobalRef + GetMethodID）。缓存读写均在本锁下进行；缓存的
// jclass 是全局引用，**只在替换缓存时释放**，切勿在每次调用里误删（会打掉别的线程正在用的方法ID）。
std::mutex g_resultMethodCacheMutex;
jclass g_cachedResultClass = nullptr;         // MainActivity 的全局引用（跨线程/跨调用复用）
jmethodID g_cachedOnResultMethod = nullptr;   // onNativeResult(String)

// 每线程 JNIEnv 缓存。结果回调固定在 native 引擎线程上被逐帧调用（30fps 量级），逐帧
// AttachCurrentThread/DetachCurrentThread 的开销可观且无必要：本线程首次回调 Attach 一次并复用，
// 线程退出时由缓存对象析构统一 Detach（仅 Detach 我们自己 Attach 的情形；env 由 Java 线程自带的
// 不 Detach）。形态与 jni/native_face_bridge.cpp 的 ThreadEnvCache 一致（本文件独立实现）。
struct ResultEnvCache {
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    bool attachedByUs = false;
    ~ResultEnvCache() {
        if (attachedByUs && vm) vm->DetachCurrentThread();
    }
};

// 取本线程可用的 JNIEnv，必要时 Attach（仅首次）。
// ⚠️ 错误码兜底：GetEnv 返回**既非 JNI_OK 又非 JNI_EDETACHED** 的错误码（如 JNI_EVERSION）时，env
// **未被初始化**，必须返回 nullptr 让调用方早退——否则会带着未初始化指针一路走到 GetObjectClass
// 崩溃。此处修复了原实现缺失该兜底的既有缺陷。
JNIEnv* acquireResultThreadEnv(JavaVM* vm) {
    static thread_local ResultEnvCache cache;
    if (vm == nullptr) return nullptr;
    if (cache.env != nullptr && cache.vm == vm) return cache.env;
    JNIEnv* env = nullptr;
    const jint st = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) return nullptr;
        cache.attachedByUs = true;
    } else if (st != JNI_OK || env == nullptr) {
        return nullptr;  // 其他错误码：env 未初始化，放弃本次上行
    } else {
        cache.attachedByUs = false;  // env 由 Java 线程自带，线程退出时不 Detach
    }
    cache.vm = vm;
    cache.env = env;
    return env;
}

}  // namespace

void sendRecognitionResult(const std::string& result) {
    if (!g_vm) return;
    JNIEnv* env = acquireResultThreadEnv(g_vm);
    if (env == nullptr) return;  // Attach 失败或 GetEnv 异常错误码：env 不可用，放弃本次上行

    // g_activity 是**全局引用**：只在锁内复制它的值并不足以在锁外安全使用——jni/engine.cpp 会在同一把
    // g_activityMutex 下 DeleteGlobalRef 旧 Activity 并换新实例，而在途回调可能正握着旧值
    // ⇒ use-after-delete。故在锁内用 NewLocalRef 取得**本线程自己的局部引用**（真正持有一份引用），
    // 用毕 DeleteLocalRef。真正的 JNI 上行调用仍在锁外进行，不放进临界区。
    jobject activityLocal = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_activityMutex);
        if (g_activity != nullptr) activityLocal = env->NewLocalRef(g_activity);
    }
    if (activityLocal == nullptr) {
        env->ExceptionClear();
        return;
    }

    // 取本实例的类（局部引用）；既作缓存失效判定的依据，也作 GetMethodID 的宿主。
    jclass localCls = env->GetObjectClass(activityLocal);
    if (localCls == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(activityLocal);
        return;
    }

    jmethodID mid = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_resultMethodCacheMutex);
        if (g_cachedResultClass != nullptr && env->IsSameObject(localCls, g_cachedResultClass)) {
            mid = g_cachedOnResultMethod;  // 类未变：复用已缓存的方法ID
        } else {
            // 先确认方法存在再替换缓存：查不到就不缓存（也清除挂起异常），本次直接放弃调用。
            jmethodID freshMid = env->GetMethodID(localCls, "onNativeResult", "(Ljava/lang/String;)V");
            if (freshMid == nullptr) {
                env->ExceptionClear();
            } else {
                jclass newGlobal = static_cast<jclass>(env->NewGlobalRef(localCls));
                if (newGlobal == nullptr) {
                    env->ExceptionClear();
                } else {
                    if (g_cachedResultClass != nullptr) env->DeleteGlobalRef(g_cachedResultClass);
                    g_cachedResultClass = newGlobal;
                    g_cachedOnResultMethod = freshMid;
                    mid = freshMid;
                }
            }
        }
    }

    env->DeleteLocalRef(localCls);

    if (mid != nullptr) {
        jstring jStr = env->NewStringUTF(result.c_str());
        if (jStr != nullptr) {
            env->CallVoidMethod(activityLocal, mid, jStr);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
            env->DeleteLocalRef(jStr);
        } else {
            env->ExceptionClear();
        }
    }

    env->DeleteLocalRef(activityLocal);
}

// ========== JNI 入口/出口 ==========

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
}
