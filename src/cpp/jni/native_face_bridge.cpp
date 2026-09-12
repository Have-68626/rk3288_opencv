/**
 * @file jni/native_face_bridge.cpp
 * @brief 人脸运行时 / 注册域 JNI（NativeFaceBridge 静态方法组，任务13）
 *
 * 职责：把人脸运行时 profile、内存人脸库与注册会话的状态机暴露给 Java 层，
 * 并把所有边界校验集中在 JNI 边界，避免非法入参越过 native 边界：
 *   - personIds 数 == embedding 数；
 *   - 每个 embedding 必须是 direct ByteBuffer 且长度精确 512*4=2048 字节；
 *   - 人脸库条目数 <= 1000；
 *   - manifest / ABI 由 FaceRuntimeManifest::parseV1 严格校验。
 * 任何非法入参或内部异常都返回 JSON 失败（takeEnrollmentTemplate 返回 null），不崩溃。
 *
 * 安全红线：模板与原始 embedding 向量**严禁写入日志**；本文件仅记录计数、状态等非敏感元数据。
 */
#include <jni.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "FaceRuntimeProfile.h"
#include "FaceSearch.h"
#include "FaceTemplate.h"
#include "JniMethodRegistry.h"
#include "NativeLog.h"
#include "jni_shared.h"

#define TAG "RK3288_JNI"

namespace {

// 512 维 ArcFace/MobileFaceNet 模板；与 Java 侧 NativeFaceBridge.EMBEDDING_DIM 保持一致。
constexpr std::size_t kEmbeddingDim = 512;
constexpr std::size_t kEmbeddingBytes = kEmbeddingDim * sizeof(float);
// 内存人脸库容量上限（任务13 边界要求）。
constexpr std::size_t kGalleryCapacity = 1000;

// 进程内单例状态。所有读写都在 mutex 下进行；注册会话模板仅驻内存，取走/取消即清零。
struct FaceBridgeState {
    std::mutex mutex;
    rk_core::FaceRuntimeProfile profile;
    bool profileLoaded = false;
    FaceSearchLinearIndex gallery;
    bool galleryLoaded = false;
    bool enrollActive = false;
    std::string enrollPersonId;
    std::vector<float> enrollEmbedding;
};

FaceBridgeState& state() {
    static FaceBridgeState s;
    return s;
}

const char* boolJson(bool v) { return v ? "true" : "false"; }

// 受限 JSON 转义：仅用于 profileId/modelVersion 等短标识，避免手写 JSON 被破坏。
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                out += (static_cast<unsigned char>(c) < 0x20) ? ' ' : c;
                break;
        }
    }
    return out;
}

std::string okJson(const std::string& kv) {
    return kv.empty() ? std::string("{\"ok\":true}") : std::string("{\"ok\":true,") + kv + "}";
}

std::string errJson(const char* code, const std::string& message) {
    if (!code) code = "FACE_UNKNOWN";
    return std::string("{\"ok\":false,\"error\":{\"code\":\"") + code +
           "\",\"message\":\"" + jsonEscape(message) + "\"}}";
}

jstring toJString(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

std::string readString(JNIEnv* env, jstring s) {
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

// 清零并释放向量，降低模板在内存中的驻留时间。
void secureClear(std::vector<float>& v) {
    std::fill(v.begin(), v.end(), 0.0f);
    v.clear();
}
void secureClear(std::vector<std::uint8_t>& v) {
    std::fill(v.begin(), v.end(), static_cast<std::uint8_t>(0));
    v.clear();
}

/**
 * 通用 RAII 清零守卫：析构时执行清理函数。
 * 用于保证明文模板/向量的局部副本在正常 return、提前 return 以及异常展开等
 * 所有路径上都被清零（仅清零，不写入任何日志）。
 */
template <typename Fn>
class ScopeClearGuard {
public:
    explicit ScopeClearGuard(Fn fn) : fn_(std::move(fn)) {}
    ~ScopeClearGuard() { fn_(); }
    ScopeClearGuard(const ScopeClearGuard&) = delete;
    ScopeClearGuard& operator=(const ScopeClearGuard&) = delete;

private:
    Fn fn_;
};

template <typename Fn>
ScopeClearGuard<Fn> makeScopeClearGuard(Fn fn) {
    return ScopeClearGuard<Fn>(std::move(fn));
}

/**
 * 读取一个 direct ByteBuffer 作为单条 512 维 embedding。
 * 校验失败时写出错误码与消息并返回 false；不记录任何向量内容。
 */
bool readDirectEmbedding(JNIEnv* env, jobject buffer, std::vector<float>& out,
                         const char*& code, std::string& message) {
    if (!buffer) {
        code = "FACE_EMB_NULL";
        message = "embedding ByteBuffer 为空";
        return false;
    }
    void* addr = env->GetDirectBufferAddress(buffer);
    const jlong capacity = env->GetDirectBufferCapacity(buffer);
    // GetDirectBufferCapacity 对非 direct buffer 返回 -1，是与 GetDirectBufferAddress==nullptr 的双重守卫。
    if (addr == nullptr || capacity < 0) {
        code = "FACE_EMB_NOT_DIRECT";
        message = "embedding 必须为 direct ByteBuffer";
        return false;
    }
    if (capacity != static_cast<jlong>(kEmbeddingBytes)) {
        code = "FACE_EMB_BAD_LENGTH";
        message = "embedding 长度必须精确为 " + std::to_string(kEmbeddingBytes) + " 字节";
        return false;
    }
    out.resize(kEmbeddingDim);
    // memcpy 规避 direct buffer 可能的未对齐访问（UB）。
    std::memcpy(out.data(), addr, kEmbeddingBytes);
    for (float v : out) {
        if (!std::isfinite(v)) {
            secureClear(out);
            code = "FACE_EMB_NON_FINITE";
            message = "embedding 含 NaN/Inf";
            return false;
        }
    }
    return true;
}

// ========== loadProfile ==========
jstring bridge_loadProfile(JNIEnv* env, jclass /* clazz */,
                           jstring manifestJson, jstring currentAbi) {
    try {
        const std::string manifest = readString(env, manifestJson);
        const std::string abi = readString(env, currentAbi);
        if (manifest.empty()) {
            return toJString(env, errJson("FACE_PROFILE_EMPTY", "manifest 为空"));
        }
        if (abi.empty()) {
            return toJString(env, errJson("FACE_PROFILE_ABI_EMPTY", "currentAbi 为空"));
        }

        rk_core::FaceRuntimeProfile profile;
        std::string err;
        if (!rk_core::FaceRuntimeManifest::parseV1(manifest, abi, profile, err)) {
            return toJString(env, errJson("FACE_PROFILE_INVALID", err));
        }

        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.profile = std::move(profile);
        s.profileLoaded = true;
        // 仅记录 profileId/modelVersion 等非敏感元数据。
        rklog::logInfo(TAG, __func__, "face runtime profile loaded");

        return toJString(env, okJson(
            "\"profileId\":\"" + jsonEscape(s.profile.profileId) +
            "\",\"modelVersion\":\"" + jsonEscape(s.profile.modelVersion) +
            "\",\"embeddingDim\":" + std::to_string(s.profile.embeddingDim)));
    } catch (const std::exception& e) {
        return toJString(env, errJson("FACE_PROFILE_EXCEPTION", e.what()));
    } catch (...) {
        return toJString(env, errJson("FACE_PROFILE_UNKNOWN", "unknown error"));
    }
}

// ========== replaceGallery ==========
jstring bridge_replaceGallery(JNIEnv* env, jclass /* clazz */,
                              jobjectArray personIds, jobjectArray embeddings) {
    try {
        if (!personIds || !embeddings) {
            return toJString(env, errJson("FACE_GALLERY_NULL", "personIds/embeddings 为空"));
        }
        const jsize idCount = env->GetArrayLength(personIds);
        const jsize embCount = env->GetArrayLength(embeddings);
        // 边界：personIds 数必须等于 embedding 数。
        if (idCount != embCount) {
            return toJString(env, errJson("FACE_GALLERY_COUNT_MISMATCH",
                "personIds(" + std::to_string(idCount) + ") 与 embeddings(" +
                std::to_string(embCount) + ") 数量不一致"));
        }
        if (idCount < 0 || static_cast<std::size_t>(idCount) > kGalleryCapacity) {
            return toJString(env, errJson("FACE_GALLERY_TOO_LARGE",
                "gallery 条目数超过上限 " + std::to_string(kGalleryCapacity)));
        }

        std::vector<FaceSearchEntry> entries;
        entries.reserve(static_cast<std::size_t>(idCount));
        // 红线：校验失败 early-return 或异常展开时，已累积条目的 embedding 也必须清零。
        auto entriesGuard = makeScopeClearGuard([&entries] {
            for (auto& e : entries) secureClear(e.embedding);
        });
        for (jsize i = 0; i < idCount; ++i) {
            jstring idRef = static_cast<jstring>(env->GetObjectArrayElement(personIds, i));
            std::string id = readString(env, idRef);
            if (idRef) env->DeleteLocalRef(idRef);
            if (id.empty()) {
                return toJString(env, errJson("FACE_GALLERY_BAD_ID", "存在空 personId"));
            }

            jobject embRef = env->GetObjectArrayElement(embeddings, i);
            std::vector<float> vec;
            // 单条 embedding 局部队列：非有限值/异常路径都不留明文。
            auto vecGuard = makeScopeClearGuard([&vec] { secureClear(vec); });
            const char* code = nullptr;
            std::string message;
            const bool ok = readDirectEmbedding(env, embRef, vec, code, message);
            if (embRef) env->DeleteLocalRef(embRef);
            if (!ok) {
                return toJString(env, errJson(code, message));
            }

            FaceSearchEntry entry;
            // 若 push_back 因异常失败，本条目 embedding 仍在此作用域内，需清零。
            auto entryGuard = makeScopeClearGuard([&entry] { secureClear(entry.embedding); });
            entry.id = std::move(id);
            entry.embedding = std::move(vec);
            entries.push_back(std::move(entry));
        }

        std::string indexErr;
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        // reset 仅在全部校验通过后替换索引；失败时保留旧库。
        if (!s.gallery.reset(std::move(entries), kEmbeddingDim, indexErr)) {
            return toJString(env, errJson("FACE_GALLERY_INVALID", indexErr));
        }
        s.galleryLoaded = true;
        rklog::logInfo(TAG, __func__, "gallery replaced count=" + std::to_string(s.gallery.size()));

        return toJString(env, okJson(
            "\"count\":" + std::to_string(s.gallery.size()) +
            ",\"dim\":" + std::to_string(s.gallery.dim())));
    } catch (const std::exception& e) {
        return toJString(env, errJson("FACE_GALLERY_EXCEPTION", e.what()));
    } catch (...) {
        return toJString(env, errJson("FACE_GALLERY_UNKNOWN", "unknown error"));
    }
}

// ========== beginEnrollment ==========
jstring bridge_beginEnrollment(JNIEnv* env, jclass /* clazz */,
                               jstring personId, jobject embedding) {
    try {
        const std::string id = readString(env, personId);
        if (id.empty()) {
            return toJString(env, errJson("FACE_ENROLL_BAD_ID", "personId 为空"));
        }

        std::vector<float> vec;
        // 红线：局部队列在失败/异常（含移入 state 之前抛错）时也必须清零。
        auto vecGuard = makeScopeClearGuard([&vec] { secureClear(vec); });
        const char* code = nullptr;
        std::string message;
        if (!readDirectEmbedding(env, embedding, vec, code, message)) {
            return toJString(env, errJson(code, message));
        }

        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        // 覆盖旧会话前先清零，避免上一人模板残留。
        secureClear(s.enrollEmbedding);
        s.enrollActive = true;
        s.enrollPersonId = id;
        s.enrollEmbedding = std::move(vec);
        // 仅记录状态，绝不记录 personId 或 embedding。
        rklog::logInfo(TAG, __func__, "enrollment session begun");

        return toJString(env, okJson("\"state\":\"ENROLLING\",\"staged\":true"));
    } catch (const std::exception& e) {
        return toJString(env, errJson("FACE_ENROLL_EXCEPTION", e.what()));
    } catch (...) {
        return toJString(env, errJson("FACE_ENROLL_UNKNOWN", "unknown error"));
    }
}

// ========== cancelEnrollment ==========
jstring bridge_cancelEnrollment(JNIEnv* env, jclass /* clazz */) {
    try {
        bool cleared = false;
        {
            auto& s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            cleared = s.enrollActive;
            s.enrollActive = false;
            s.enrollPersonId.clear();
            secureClear(s.enrollEmbedding);
        }
        rklog::logInfo(TAG, __func__, "enrollment session cancelled");
        return toJString(env, okJson(
            std::string("\"state\":\"IDLE\",\"cleared\":") + boolJson(cleared)));
    } catch (const std::exception& e) {
        return toJString(env, errJson("FACE_ENROLL_CANCEL_EXCEPTION", e.what()));
    } catch (...) {
        return toJString(env, errJson("FACE_ENROLL_CANCEL_UNKNOWN", "unknown error"));
    }
}

// ========== getEnrollmentStatus ==========
jstring bridge_getEnrollmentStatus(JNIEnv* env, jclass /* clazz */) {
    try {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        // 只回状态与 personId，不回模板内容。
        return toJString(env, okJson(
            std::string("\"state\":\"") + (s.enrollActive ? "ENROLLING" : "IDLE") +
            "\",\"staged\":" + boolJson(s.enrollActive) +
            ",\"personId\":\"" + jsonEscape(s.enrollPersonId) + "\""));
    } catch (const std::exception& e) {
        return toJString(env, errJson("FACE_ENROLL_STATUS_EXCEPTION", e.what()));
    } catch (...) {
        return toJString(env, errJson("FACE_ENROLL_STATUS_UNKNOWN", "unknown error"));
    }
}

// ========== takeEnrollmentTemplate ==========
jbyteArray bridge_takeEnrollmentTemplate(JNIEnv* env, jclass /* clazz */) {
    try {
        std::vector<std::uint8_t> bytes;
        // 红线：序列化模板字节副本必须覆盖 NewByteArray 失败、异常、提前 return 等所有路径。
        auto bytesGuard = makeScopeClearGuard([&bytes] { secureClear(bytes); });
        {
            auto& s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.enrollActive || s.enrollEmbedding.size() != kEmbeddingDim) {
                return nullptr;
            }

            FaceTemplate t;
            // t.embedding 是 s.enrollEmbedding 的拷贝，离开作用域（含异常展开）必须清零。
            auto templateGuard = makeScopeClearGuard([&t] { secureClear(t.embedding); });
            t.header.dim = static_cast<std::uint16_t>(kEmbeddingDim);
            t.header.payloadBytes = static_cast<std::uint32_t>(kEmbeddingBytes);
            t.embedding = s.enrollEmbedding;
            bytes = serializeFaceTemplate(t);

            // 一次性语义：取走即清空会话与内存中的模板。
            s.enrollActive = false;
            s.enrollPersonId.clear();
            secureClear(s.enrollEmbedding);
        }

        if (bytes.empty()) return nullptr;
        jbyteArray out = env->NewByteArray(static_cast<jsize>(bytes.size()));
        if (!out) return nullptr;
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(bytes.size()),
                                reinterpret_cast<const jbyte*>(bytes.data()));
        // bytesGuard 于函数作用域退出时清零 bytes，覆盖上述提前 return 与异常路径。
        // 仅记录“模板已取走”，不记录字节数之外的内容。
        rklog::logInfo(TAG, __func__, "enrollment template consumed");
        return out;
    } catch (const std::exception&) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

// ========== getRuntimeStatus ==========
jstring bridge_getRuntimeStatus(JNIEnv* env, jclass /* clazz */) {
    try {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const std::size_t galleryCount = s.galleryLoaded ? s.gallery.size() : 0;
        const std::size_t galleryDim = s.galleryLoaded ? s.gallery.dim() : 0;
        return toJString(env, okJson(
            std::string("\"profileLoaded\":") + boolJson(s.profileLoaded) +
            ",\"profileId\":\"" + jsonEscape(s.profileLoaded ? s.profile.profileId : std::string()) + "\"" +
            ",\"embeddingDim\":" + std::to_string(s.profileLoaded ? s.profile.embeddingDim : 0) +
            ",\"galleryLoaded\":" + boolJson(s.galleryLoaded) +
            ",\"galleryCount\":" + std::to_string(galleryCount) +
            ",\"galleryDim\":" + std::to_string(galleryDim) +
            ",\"enrollmentState\":\"" + (s.enrollActive ? "ENROLLING" : "IDLE") + "\""));
    } catch (const std::exception& e) {
        return toJString(env, errJson("FACE_RUNTIME_STATUS_EXCEPTION", e.what()));
    } catch (...) {
        return toJString(env, errJson("FACE_RUNTIME_STATUS_UNKNOWN", "unknown error"));
    }
}

}  // anonymous namespace

void registerFaceBridgeMethods(JniMethodRegistry& registry) {
    registry.add({
        {"loadProfile",
         "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
         reinterpret_cast<void*>(bridge_loadProfile)},
        {"replaceGallery",
         "([Ljava/lang/String;[Ljava/nio/ByteBuffer;)Ljava/lang/String;",
         reinterpret_cast<void*>(bridge_replaceGallery)},
        {"beginEnrollment",
         "(Ljava/lang/String;Ljava/nio/ByteBuffer;)Ljava/lang/String;",
         reinterpret_cast<void*>(bridge_beginEnrollment)},
        {"cancelEnrollment",
         "()Ljava/lang/String;",
         reinterpret_cast<void*>(bridge_cancelEnrollment)},
        {"getEnrollmentStatus",
         "()Ljava/lang/String;",
         reinterpret_cast<void*>(bridge_getEnrollmentStatus)},
        {"takeEnrollmentTemplate",
         "()[B",
         reinterpret_cast<void*>(bridge_takeEnrollmentTemplate)},
        {"getRuntimeStatus",
         "()Ljava/lang/String;",
         reinterpret_cast<void*>(bridge_getRuntimeStatus)},
    });
}
