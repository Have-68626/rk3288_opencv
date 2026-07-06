/**
 * @file jni/config.cpp
 * @brief JNI 配置与工具函数（config domain）
 *
 * 包括日志配置、独立推理、运行时节流参数更新等 NativeBridge 静态方法。
 */
#include <jni.h>
#include <string>

#include "Engine.h"
#include "FaceInferencePipeline.h"
#include "NativeLog.h"
#include "JniMethodRegistry.h"
#include "jni_shared.h"

#define TAG "RK3288_JNI"

namespace {

// ========== JSON 字符串转义 ==========
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += ' ';
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

// ========== nativeConfigureLog ==========
void config_nativeConfigureLog(
    JNIEnv* env, jclass /* clazz */,
    jstring internalDir, jstring externalDir, jstring filename) {

    const char* in = internalDir ? env->GetStringUTFChars(internalDir, nullptr) : nullptr;
    const char* ex = externalDir ? env->GetStringUTFChars(externalDir, nullptr) : nullptr;
    const char* fn = filename ? env->GetStringUTFChars(filename, nullptr) : nullptr;

    std::string inStr = in ? in : "";
    std::string exStr = ex ? ex : "";
    std::string fnStr = fn ? fn : "rk3288.log";

    if (in) env->ReleaseStringUTFChars(internalDir, in);
    if (ex) env->ReleaseStringUTFChars(externalDir, ex);
    if (fn) env->ReleaseStringUTFChars(filename, fn);

    rklog::setLogDirs(inStr, exStr, fnStr);
    rklog::logInfo(TAG, __func__, "nativeConfigureLog configured: " + fnStr);
}

// ========== nativeInferFaceFromImage ==========
jstring config_nativeInferFaceFromImage(
    JNIEnv* env, jclass /* clazz */,
    jstring imagePath, jstring yoloModelPath, jstring arcModelPath,
    jstring galleryDir, jint topK, jfloat threshold,
    jboolean fakeDetect, jboolean fakeEmbedding) {

    try {
        const char* img = imagePath ? env->GetStringUTFChars(imagePath, nullptr) : nullptr;
        const char* yolo = yoloModelPath ? env->GetStringUTFChars(yoloModelPath, nullptr) : nullptr;
        const char* arc = arcModelPath ? env->GetStringUTFChars(arcModelPath, nullptr) : nullptr;
        const char* gal = galleryDir ? env->GetStringUTFChars(galleryDir, nullptr) : nullptr;

        rk_core::FaceInferRequest req;
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

        const auto o = rk_core::runFaceInferOnce(req);
        return env->NewStringUTF(o.json.c_str());
    } catch (const std::exception& e) {
        return env->NewStringUTF((
            "{\"ok\":false,\"error\":{\"code\":\"JNI_INFER_FAILED\",\"message\":\""
            + jsonEscape(e.what()) + "\"}}").c_str());
    } catch (...) {
        return env->NewStringUTF(
            "{\"ok\":false,\"error\":{\"code\":\"JNI_UNKNOWN\",\"message\":\"Unknown JNI error\"}}");
    }
}

// ========== 运行时节流参数更新 ==========

void config_nativeSetInferenceThrottle(
    JNIEnv* env, jclass /* clazz */,
    jstring mode, jint intervalMs) {
    if (!g_engine) return;
    const char* m = mode ? env->GetStringUTFChars(mode, nullptr) : nullptr;
    std::string modeStr = m ? m : "off";
    if (m) env->ReleaseStringUTFChars(mode, m);
    // JNI 边界：该接口只更新 Engine 内的原子配置，确保运行中更新不会引入锁竞争或悬挂引用。
    g_engine->updateInferenceThrottle(modeStr, static_cast<int>(intervalMs));
}

void config_nativeSetDetectionThrottle(
    JNIEnv* env, jclass /* clazz */,
    jstring mode, jint intervalMs) {
    if (!g_engine) return;
    const char* m = mode ? env->GetStringUTFChars(mode, nullptr) : nullptr;
    std::string modeStr = m ? m : "off";
    if (m) env->ReleaseStringUTFChars(mode, m);
    g_engine->updateDetectionThrottle(modeStr, static_cast<int>(intervalMs));
}

void config_nativeSetRecognitionThrottle(
    JNIEnv* env, jclass /* clazz */,
    jstring mode, jint intervalMs) {
    if (!g_engine) return;
    const char* m = mode ? env->GetStringUTFChars(mode, nullptr) : nullptr;
    std::string modeStr = m ? m : "off";
    if (m) env->ReleaseStringUTFChars(mode, m);
    g_engine->updateRecognitionThrottle(modeStr, static_cast<int>(intervalMs));
}

} // anonymous namespace

void registerConfigMethods(JniMethodRegistry& registry) {
    registry.add({
        {"nativeConfigureLog",
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
         reinterpret_cast<void*>(config_nativeConfigureLog)},
        {"nativeInferFaceFromImage",
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IFZZ)Ljava/lang/String;",
         reinterpret_cast<void*>(config_nativeInferFaceFromImage)},
        {"nativeSetInferenceThrottle",
         "(Ljava/lang/String;I)V",
         reinterpret_cast<void*>(config_nativeSetInferenceThrottle)},
        {"nativeSetDetectionThrottle",
         "(Ljava/lang/String;I)V",
         reinterpret_cast<void*>(config_nativeSetDetectionThrottle)},
        {"nativeSetRecognitionThrottle",
         "(Ljava/lang/String;I)V",
         reinterpret_cast<void*>(config_nativeSetRecognitionThrottle)},
    });
}
