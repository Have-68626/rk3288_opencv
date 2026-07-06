#pragma once

#include <jni.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <string>

struct ANativeWindow;
class JniMethodRegistry;

namespace rk_core {
class Engine;
} // namespace rk_core

// ========== 共享全局变量（定义在 native-lib.cpp） ==========
extern std::unique_ptr<rk_core::Engine> g_engine;
extern std::thread g_engineThread;
extern std::mutex g_engineThreadMutex;
extern JavaVM* g_vm;
extern jobject g_activity;
extern std::mutex g_activityMutex;
extern std::mutex g_previewMutex;
extern ANativeWindow* g_previewWindow;
extern std::atomic<uint64_t> g_previewGeneration;
extern std::atomic<bool> g_cancelInit;

// ========== 共享辅助函数（定义在 native-lib.cpp） ==========
void stopAndJoinEngineThreadIfRunning();
void sendRecognitionResult(const std::string& result);

// ========== 领域注册函数声明（定义在各 jni/*.cpp） ==========
void registerCameraMethods(JniMethodRegistry& registry);
void registerEngineMethods(JniMethodRegistry& registry);
void registerPreviewMethods(JniMethodRegistry& registry);
void registerConfigMethods(JniMethodRegistry& registry);

// ========== 全局注册入口（定义在 jni/registry.cpp） ==========
jint registerAllNativeMethods(JNIEnv* env);
