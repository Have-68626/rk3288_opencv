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

// JNI 挂起异常后只能释放已有引用；清除后早退，不能继续读取后续字符串。
bool readUtf(JNIEnv* env, jstring value, std::string& out) {
    if (!value) return true;
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (env->ExceptionCheck()) {
        if (chars) env->ReleaseStringUTFChars(value, chars);
        env->ExceptionClear();
        return false;
    }
    if (!chars) return false;
    try {
        out = chars;
    } catch (...) {
        env->ReleaseStringUTFChars(value, chars);
        throw;
    }
    env->ReleaseStringUTFChars(value, chars);
    return true;
}

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

    std::string inStr, exStr, fnStr = "rk3288.log";
    if (!readUtf(env, internalDir, inStr) || !readUtf(env, externalDir, exStr) ||
        !readUtf(env, filename, fnStr)) return;

    rklog::setLogDirs(inStr, exStr, fnStr);
    rklog::logInfo(TAG, __func__, "nativeConfigureLog configured: " + fnStr);
}

// ========== nativeInferFaceFromImage ==========
jstring config_nativeInferFaceFromImage(
    JNIEnv* env, jclass /* clazz */,
    jstring imagePath, jint topK) {

    try {
        rk_core::FaceInferRequest req;
        if (!readUtf(env, imagePath, req.imagePath)) {
            return env->NewStringUTF("{\"ok\":false,\"message\":\"图像路径读取失败\"}");
        }
        req.topK = topK > 0 ? static_cast<std::size_t>(topK) : 5;
        req.thresholdVersionId = "thr_v1";
        std::string reason;
        if (!bindActiveFaceInferRequest(req, reason)) {
            return env->NewStringUTF("{\"ok\":false,\"stage\":\"face_profile\",\"message\":\"请先激活模型\"}");
        }

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
    std::string modeStr = "off";
    if (!readUtf(env, mode, modeStr)) return;
    // JNI 边界：该接口只更新 Engine 内的原子配置，确保运行中更新不会引入锁竞争或悬挂引用。
    g_engine->updateInferenceThrottle(modeStr, static_cast<int>(intervalMs));
}

void config_nativeSetDetectionThrottle(
    JNIEnv* env, jclass /* clazz */,
    jstring mode, jint intervalMs) {
    if (!g_engine) return;
    std::string modeStr = "off";
    if (!readUtf(env, mode, modeStr)) return;
    g_engine->updateDetectionThrottle(modeStr, static_cast<int>(intervalMs));
}

void config_nativeSetRecognitionThrottle(
    JNIEnv* env, jclass /* clazz */,
    jstring mode, jint intervalMs) {
    if (!g_engine) return;
    std::string modeStr = "off";
    if (!readUtf(env, mode, modeStr)) return;
    g_engine->updateRecognitionThrottle(modeStr, static_cast<int>(intervalMs));
}

} // anonymous namespace

void registerConfigMethods(JniMethodRegistry& registry) {
    registry.add({
        {"nativeConfigureLog",
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
         reinterpret_cast<void*>(config_nativeConfigureLog)},
        {"nativeInferFaceFromImage",
         "(Ljava/lang/String;I)Ljava/lang/String;",
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
