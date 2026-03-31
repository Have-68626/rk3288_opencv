#include <jni.h>
#include <string>
#include <thread>
#include <mutex>
#include <android/log.h>
#include <android/bitmap.h>
#include "Engine.h"
#include "FaceInferencePipeline.h"
#include "NativeLog.h"

#define TAG "RK3288_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Global Engine Instance
static std::unique_ptr<Engine> g_engine;
static std::thread g_engineThread;
static std::mutex g_engineThreadMutex;
static JavaVM* g_vm = nullptr;
static jobject g_activity = nullptr;

static void stopAndJoinEngineThreadIfRunning() {
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
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

void sendRecognitionResult(const std::string& result) {
    if (!g_vm || !g_activity) return;

    JNIEnv* env;
    int getEnvStat = g_vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    bool attached = false;

    if (getEnvStat == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != 0) {
            return;
        }
        attached = true;
    }

    jclass cls = env->GetObjectClass(g_activity);
    jmethodID mid = env->GetMethodID(cls, "onNativeResult", "(Ljava/lang/String;)V");
    if (mid) {
        jstring jStr = env->NewStringUTF(result.c_str());
        env->CallVoidMethod(g_activity, mid, jStr);
        env->DeleteLocalRef(jStr);
    }

    // Clean up local ref to class not strictly needed but good practice
    env->DeleteLocalRef(cls);

    if (attached) {
        g_vm->DetachCurrentThread();
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeInitFile(
        JNIEnv* env,
        jobject thiz,
        jstring filePath,
        jstring cascadePath,
        jstring storagePath) {
    RKLOG_ENTER(TAG);

    stopAndJoinEngineThreadIfRunning();

    // Update global activity ref
    if (g_activity) env->DeleteGlobalRef(g_activity);
    g_activity = env->NewGlobalRef(thiz);
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
        g_engine = std::make_unique<Engine>();
    }

    g_engine->setOnResultCallback(sendRecognitionResult);
    return g_engine->initialize(pathStr, cascadeStr, storageStr);
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
Java_com_example_rk3288_1opencv_NativeBridge_nativeInferFaceFromImage(
        JNIEnv* env,
        jclass,
        jstring imagePath,
        jstring yoloModelPath,
        jstring arcModelPath,
        jstring galleryDir,
        jint topK,
        jfloat threshold,
        jboolean fakeDetect,
        jboolean fakeEmbedding) {
    const char* img = imagePath ? env->GetStringUTFChars(imagePath, nullptr) : nullptr;
    const char* yolo = yoloModelPath ? env->GetStringUTFChars(yoloModelPath, nullptr) : nullptr;
    const char* arc = arcModelPath ? env->GetStringUTFChars(arcModelPath, nullptr) : nullptr;
    const char* gal = galleryDir ? env->GetStringUTFChars(galleryDir, nullptr) : nullptr;

    FaceInferRequest req;
    req.imagePath = img ? img : "";
    req.yoloBackend = "opencv";
    req.yoloModelPath = yolo ? yolo : "";
    req.arcBackend = "opencv";
    req.arcModelPath = arc ? arc : "";
    req.galleryDir = gal ? gal : "";
    req.topK = topK > 0 ? static_cast<std::size_t>(topK) : 5;
    req.acceptThreshold = static_cast<float>(threshold);
    req.thresholdVersionId = "thr_v1";
    req.fakeDetect = (fakeDetect == JNI_TRUE);
    req.fakeEmbedding = (fakeEmbedding == JNI_TRUE);

    if (img) env->ReleaseStringUTFChars(imagePath, img);
    if (yolo) env->ReleaseStringUTFChars(yoloModelPath, yolo);
    if (arc) env->ReleaseStringUTFChars(arcModelPath, arc);
    if (gal) env->ReleaseStringUTFChars(galleryDir, gal);

    const auto o = runFaceInferOnce(req);
    return env->NewStringUTF(o.json.c_str());
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
        jobject thiz,
        jint cameraId,
        jstring cascadePath,
        jstring storagePath) {
    RKLOG_ENTER(TAG);

    stopAndJoinEngineThreadIfRunning();

    // Update global activity ref
    if (g_activity) env->DeleteGlobalRef(g_activity);
    g_activity = env->NewGlobalRef(thiz);
    const char* cascade = cascadePath ? env->GetStringUTFChars(cascadePath, nullptr) : nullptr;
    const char* storage = storagePath ? env->GetStringUTFChars(storagePath, nullptr) : nullptr;

    std::string cascadeStr = cascade ? cascade : "";
    std::string storageStr = storage ? storage : "";

    if (cascade) env->ReleaseStringUTFChars(cascadePath, cascade);
    if (storage) env->ReleaseStringUTFChars(storagePath, storage);

    LOGI("Initializing Engine with Camera ID: %d...", cameraId);
    if (!g_engine) {
        g_engine = std::make_unique<Engine>();
    }

    g_engine->setOnResultCallback(sendRecognitionResult);
    return g_engine->initialize(cameraId, cascadeStr, storageStr);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeStart(
        JNIEnv* env,
        jobject /* this */) {
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

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeStop(
        JNIEnv* env,
        jobject /* this */) {
    RKLOG_ENTER(TAG);
    LOGI("Stopping Engine...");
    stopAndJoinEngineThreadIfRunning();
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
