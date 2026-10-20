// 由宿主 JNI 函数表驱动真实包装层；只替换 JVM/Android 边界，不调用真实相机。
#ifndef ENGINE_WRAPPER_SOURCE
#define ENGINE_WRAPPER_SOURCE "../../src/cpp/jni/engine.cpp"
#endif
#include ENGINE_WRAPPER_SOURCE

#include <iostream>
#include <stdexcept>

std::unique_ptr<rk_core::Engine> g_engine;
std::thread g_engineThread;
std::mutex g_engineThreadMutex, g_activityMutex;
jobject g_activity = nullptr;
std::atomic<bool> g_cancelInit{false};
void stopAndJoinEngineThreadIfRunning() {}
void sendRecognitionResult(const std::string&) {}
void applyPendingFaceRuntimeActivation() {}
void JniMethodRegistry::add(std::initializer_list<JniMethodDef>) {}

static bool pending = false, failReference = false;
static int reads = 0, failAt = 0, releases = 0, clears = 0, deleted = 0;
static const char* JNICALL getChars(JNIEnv*, jstring, jboolean*) {
    if (pending) throw std::runtime_error("挂起异常后仍调用字符串读取");
    if (++reads == failAt) { pending = true; return nullptr; }
    return "cache/review-495-test-temp/jni_missing_fixture";
}
static void JNICALL releaseChars(JNIEnv*, jstring, const char*) { ++releases; }
static jboolean JNICALL exceptionCheck(JNIEnv*) { return pending; }
static void JNICALL exceptionClear(JNIEnv*) { pending = false; ++clears; }
static jobject JNICALL newGlobal(JNIEnv*, jobject object) {
    if (failReference) { pending = true; return nullptr; }
    return object;
}
static void JNICALL deleteGlobal(JNIEnv*, jobject) { ++deleted; }

int main() {
    JNINativeInterface_ functions{};
    functions.GetStringUTFChars = getChars;
    functions.ReleaseStringUTFChars = releaseChars;
    functions.ExceptionCheck = exceptionCheck;
    functions.ExceptionClear = exceptionClear;
    functions.NewGlobalRef = newGlobal;
    functions.DeleteGlobalRef = deleteGlobal;
    JNIEnv env{&functions};
    const auto text = reinterpret_cast<jstring>(1);
    const auto activity = reinterpret_cast<jobject>(2);
    int failures = 0;
    for (int method = 0; method < 2; ++method) {
        for (int at = 1; at <= 3; ++at) {
            reads = releases = clears = deleted = 0;
            pending = failReference = false;
            failAt = at;
            g_engine.reset();
            try {
                const auto result = method == 0 ? engine_nativeInitFile(&env, activity, text, text, text)
                                                : engine_nativeInit(&env, activity, text, text, text);
                if (result != JNI_FALSE || reads != at || releases != at - 1 || clears != 1 || pending || g_engine) ++failures;
            } catch (...) { ++failures; }
        }
        reads = releases = clears = deleted = 0;
        pending = false;
        failReference = true;
        const auto previous = reinterpret_cast<jobject>(3);
        g_activity = previous;
        try {
            const auto result = method == 0 ? engine_nativeInitFile(&env, activity, text, text, text)
                                            : engine_nativeInit(&env, activity, text, text, text);
            if (result != JNI_FALSE || reads != 0 || clears != 1 || pending || deleted != 0 || g_activity != previous) ++failures;
        } catch (...) { ++failures; }
    }
    g_activity = nullptr;
    g_engine.reset();
    std::cout << "JNI_EXCEPTION_PROBE failures=" << failures << " total=8\n";
    return failures == 0 ? 0 : 1;
}
