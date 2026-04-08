#include <jni.h>
#include <string>
#include <thread>
#include <mutex>
#include <cstring>
#include <android/log.h>
#include <android/bitmap.h>
#include <android/native_window_jni.h>
#include <opencv2/imgproc.hpp>
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
static std::mutex g_previewMutex;
static ANativeWindow* g_previewWindow = nullptr;

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
    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        if (g_previewWindow) {
            ANativeWindow_release(g_previewWindow);
            g_previewWindow = nullptr;
        }
    }
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    std::lock_guard<std::mutex> lock(g_previewMutex);
    if (g_previewWindow) {
        ANativeWindow_release(g_previewWindow);
        g_previewWindow = nullptr;
    }
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

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeConfigureExternalInput(
        JNIEnv* env,
        jobject /* this */,
        jboolean enabled,
        jint backpressureMode,
        jint queueCapacity) {
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

namespace {
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
}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativePushFrameNV21(
        JNIEnv* env,
        jobject /* this */,
        jobject nv21Buffer,
        jint width,
        jint height,
        jint rowStrideY,
        jlong timestampNs,
        jint rotationDegrees,
        jboolean mirrored) {
    if (!g_engine) return JNI_FALSE;

    ExternalFrame f;
    f.format = ExternalFrameFormat::NV21;
    f.width = width;
    f.height = height;
    f.meta.timestampNs = static_cast<int64_t>(timestampNs);
    f.meta.rotationDegrees = rotationDegrees;
    f.meta.mirrored = (mirrored == JNI_TRUE);
    f.nv21RowStrideY = rowStrideY > 0 ? rowStrideY : width;

    if (width <= 0 || height <= 0 || f.nv21RowStrideY < width) {
        rklog::logWarn(TAG, __func__, "nativePushFrameNV21 参数非法");
        return JNI_FALSE;
    }

    const std::size_t needY = static_cast<std::size_t>(f.nv21RowStrideY) * static_cast<std::size_t>(height);
    const std::size_t needUV = static_cast<std::size_t>(f.nv21RowStrideY) * static_cast<std::size_t>((height + 1) / 2);
    const std::size_t need = needY + needUV;

    // JNI 所有权：这里做深拷贝，保证 Java 侧复用/释放 Buffer 不会导致 Native 悬挂指针。
    if (!copyDirectBytes(env, nv21Buffer, need, f.nv21)) {
        rklog::logWarn(TAG, __func__, "nativePushFrameNV21 需要 DirectByteBuffer 且容量足够");
        return JNI_FALSE;
    }

    const bool ok = g_engine->pushExternalFrame(std::move(f));
    if (!ok) {
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativePushFrameYuv420888(
        JNIEnv* env,
        jobject /* this */,
        jobject yBuffer,
        jint yRowStride,
        jobject uBuffer,
        jint uRowStride,
        jint uPixelStride,
        jobject vBuffer,
        jint vRowStride,
        jint vPixelStride,
        jint width,
        jint height,
        jlong timestampNs,
        jint rotationDegrees,
        jboolean mirrored) {
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
    // 最小可用骨架阶段：约定 Java 侧传入前先 rewind()/position(0)，避免拷贝到错误偏移。
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

    const bool ok = g_engine->pushExternalFrame(std::move(f));
    if (!ok) {
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativePushFrameNV21Bytes(
        JNIEnv* env,
        jobject /* this */,
        jbyteArray nv21Bytes,
        jint width,
        jint height,
        jint rowStrideY,
        jlong timestampNs,
        jint rotationDegrees,
        jboolean mirrored) {
    if (!g_engine) return JNI_FALSE;
    if (!nv21Bytes) return JNI_FALSE;

    ExternalFrame f;
    f.format = ExternalFrameFormat::NV21;
    f.width = width;
    f.height = height;
    f.meta.timestampNs = static_cast<int64_t>(timestampNs);
    f.meta.rotationDegrees = rotationDegrees;
    f.meta.mirrored = (mirrored == JNI_TRUE);
    f.nv21RowStrideY = rowStrideY > 0 ? rowStrideY : width;

    if (width <= 0 || height <= 0 || f.nv21RowStrideY < width) {
        rklog::logWarn(TAG, __func__, "nativePushFrameNV21Bytes 参数非法");
        return JNI_FALSE;
    }

    const std::size_t needY = static_cast<std::size_t>(f.nv21RowStrideY) * static_cast<std::size_t>(height);
    const std::size_t needUV = static_cast<std::size_t>(f.nv21RowStrideY) * static_cast<std::size_t>((height + 1) / 2);
    const std::size_t need = needY + needUV;

    const jsize len = env->GetArrayLength(nv21Bytes);
    if (len < 0 || static_cast<std::size_t>(len) < need) {
        rklog::logWarn(TAG, __func__, "nativePushFrameNV21Bytes 缓冲区长度不足");
        return JNI_FALSE;
    }

    f.nv21.resize(need);
    env->GetByteArrayRegion(nv21Bytes, 0, static_cast<jsize>(need),
                            reinterpret_cast<jbyte*>(f.nv21.data()));

    return g_engine->pushExternalFrame(std::move(f)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativePushFrameYuv420888Bytes(
        JNIEnv* env,
        jobject /* this */,
        jbyteArray yBytes,
        jint yRowStride,
        jbyteArray uBytes,
        jint uRowStride,
        jint uPixelStride,
        jbyteArray vBytes,
        jint vRowStride,
        jint vPixelStride,
        jint width,
        jint height,
        jlong timestampNs,
        jint rotationDegrees,
        jboolean mirrored) {
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

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeSetPreviewSurface(
        JNIEnv* env,
        jobject /* this */,
        jobject surface) {
    std::lock_guard<std::mutex> lock(g_previewMutex);
    if (g_previewWindow) {
        ANativeWindow_release(g_previewWindow);
        g_previewWindow = nullptr;
    }
    if (!surface) return;
    g_previewWindow = ANativeWindow_fromSurface(env, surface);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeRenderFrameToSurface(
        JNIEnv* /* env */,
        jobject /* this */) {
    if (!g_engine) return JNI_FALSE;

    ANativeWindow* win = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        win = g_previewWindow;
    }
    if (!win) return JNI_FALSE;

    cv::Mat frame;
    if (!g_engine->getRenderFrame(frame)) {
        return JNI_FALSE;
    }
    if (frame.empty()) return JNI_FALSE;

    thread_local cv::Mat rgba;
    cv::cvtColor(frame, rgba, cv::COLOR_BGR2RGBA);

    ANativeWindow_setBuffersGeometry(win, rgba.cols, rgba.rows, WINDOW_FORMAT_RGBA_8888);

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(win, &buffer, nullptr) != 0) {
        return JNI_FALSE;
    }

    const int w = rgba.cols;
    const int h = rgba.rows;
    const int dstStrideBytes = buffer.stride * 4;
    for (int row = 0; row < h; row++) {
        uint8_t* dst = reinterpret_cast<uint8_t*>(buffer.bits) + row * dstStrideBytes;
        const uint8_t* src = rgba.ptr<uint8_t>(row);
        std::memcpy(dst, src, static_cast<std::size_t>(w) * 4U);
    }

    ANativeWindow_unlockAndPost(win);
    return JNI_TRUE;
}
