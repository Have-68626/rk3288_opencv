#include <jni.h>
#include <string>
#include <thread>
#include <android/log.h>
#include <android/bitmap.h>
#include "Engine.h"
#include "NativeLog.h"

#define TAG "RK3288_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Global Engine Instance
static std::unique_ptr<Engine> g_engine;
static std::thread g_engineThread;

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeInitFile(
        JNIEnv* env,
        jobject /* this */,
        jstring filePath) {
    RKLOG_ENTER(TAG);
    const char* path = filePath ? env->GetStringUTFChars(filePath, nullptr) : nullptr;
    std::string pathStr = path ? path : "";
    if (path) env->ReleaseStringUTFChars(filePath, path);
    
    LOGI("Initializing Engine with Mock File: %s...", pathStr.c_str());
    if (!g_engine) {
        g_engine = std::make_unique<Engine>();
    }
    return g_engine->initialize(pathStr);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_NativeBridge_nativeConfigureLog(
        JNIEnv* env,
        jclass,
        jstring internalDir,
        jstring externalDir,
        jstring filename) {
    const char* in = internalDir ? env->GetStringUTFChars(internalDir, nullptr) : nullptr;
    const char* ex = externalDir ? env->GetStringUTFChars(externalDir, nullptr) : nullptr;
    const char* fn = filename ? env->GetStringUTFChars(filename, nullptr) : nullptr;
    
    std::string inStr = in ? in : "";
    std::string exStr = ex ? ex : "";
    std::string fnStr = fn ? fn : "rk3288.log"; // Fallback
    
    if (in) env->ReleaseStringUTFChars(internalDir, in);
    if (ex) env->ReleaseStringUTFChars(externalDir, ex);
    if (fn) env->ReleaseStringUTFChars(filename, fn);
    
    rklog::setLogDirs(inStr, exStr, fnStr);
    rklog::logInfo(TAG, __func__, "nativeConfigureLog configured: " + fnStr);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    RKLOG_ENTER(TAG);
    std::string hello = "Hello from RK3288 C++ Engine";
    return env->NewStringUTF(hello.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeInit(
        JNIEnv* env,
        jobject /* this */,
        jint cameraId) {
    RKLOG_ENTER(TAG);
    LOGI("Initializing Engine with Camera ID: %d...", cameraId);
    if (!g_engine) {
        g_engine = std::make_unique<Engine>();
    }
    return g_engine->initialize(cameraId);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeStart(
        JNIEnv* env,
        jobject /* this */) {
    RKLOG_ENTER(TAG);
    LOGI("Starting Engine...");
    if (g_engine) {
        // Start engine in a separate thread
        g_engineThread = std::thread([]() {
            g_engine->run();
        });
        g_engineThread.detach(); // Detach so it runs independently
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeStop(
        JNIEnv* env,
        jobject /* this */) {
    RKLOG_ENTER(TAG);
    LOGI("Stopping Engine...");
    if (g_engine) {
        g_engine->stop();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeSetMode(
        JNIEnv* env,
        jobject /* this */,
        jint mode) {
    RKLOG_ENTER(TAG);
    if (g_engine) {
        g_engine->setMode(static_cast<MonitoringMode>(mode));
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeGetFrame(
        JNIEnv* env,
        jobject /* this */,
        jobject bitmap) {
    if (!g_engine) return false;

    cv::Mat frame;
    if (!g_engine->getRenderFrame(frame)) {
        return false;
    }

    // Lock Bitmap pixels
    AndroidBitmapInfo info;
    void* pixels;
    
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) return false;
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) return false;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0) return false;

    // Convert OpenCV BGR to RGBA for Bitmap
    // Ensure frame size matches bitmap
    cv::Mat tmp;
    if (frame.cols != info.width || frame.rows != info.height) {
        cv::resize(frame, tmp, cv::Size(info.width, info.height));
    } else {
        tmp = frame;
    }

    cv::Mat rgbaFrame(info.height, info.width, CV_8UC4, pixels);
    cv::cvtColor(tmp, rgbaFrame, cv::COLOR_BGR2RGBA);

    AndroidBitmap_unlockPixels(env, bitmap);
    return true;
}
