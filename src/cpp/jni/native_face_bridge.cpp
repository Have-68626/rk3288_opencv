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

#include "Engine.h"
#include "FaceRuntimeProfile.h"
#include "FaceSearch.h"
#include "FaceTemplate.h"
#include "JniMethodRegistry.h"
#include "NativeLog.h"
#include "jni_shared.h"
#include "pipeline/RealtimeFacePipeline.h"

#define TAG "RK3288_JNI"

namespace {

// 512 维 ArcFace/MobileFaceNet 模板；与 Java 侧 NativeFaceBridge.EMBEDDING_DIM 保持一致。
constexpr std::size_t kEmbeddingDim = 512;
constexpr std::size_t kEmbeddingBytes = kEmbeddingDim * sizeof(float);
// 内存人脸库容量上限（任务13 边界要求）。
constexpr std::size_t kGalleryCapacity = 1000;

// 不可变图库快照槽：reload 的 loadGallery 钩子与 replaceGallery 更新共用同一槽。
// 槽内只放 shared_ptr<const FaceSearchLinearIndex>，读侧加锁复制指针后即可在锁外安全使用，
// 避免「reload 读可变索引」与「replaceGallery 替换索引」并发导致的数据竞争。
struct GallerySlot {
    std::mutex mutex;
    std::shared_ptr<const FaceSearchLinearIndex> snapshot;
};

// 进程内单例状态。所有读写都在 mutex 下进行；注册会话模板仅驻内存，取走/取消即清零。
struct FaceBridgeState {
    std::mutex mutex;
    rk_core::FaceRuntimeProfile profile;
    // 仅表示 manifest 已通过格式/ABI 校验并暂存，**不代表**推理管线已加载生效。
    // 真正的「已加载生效」由 Engine 实时快照的 effective/reason 表达（getRuntimeStatus）。
    bool profileValidated = false;
    FaceSearchLinearIndex gallery;
    bool galleryLoaded = false;
    std::shared_ptr<GallerySlot> gallerySlot = std::make_shared<GallerySlot>();
    bool enrollActive = false;
    std::string enrollPersonId;
    std::vector<float> enrollEmbedding;

    // Engine 尚未创建时的待激活参数（activateRuntime 先于 nativeInit 调用）：
    // engine.cpp 创建 Engine 后经 applyPendingFaceRuntimeActivation() 一次性消费。
    bool pendingActivation = false;
    std::string pendingManifest;
    std::string pendingBaseDir;
    std::string pendingAbi;
    std::string pendingGalleryDir;
};

FaceBridgeState& state() {
    static FaceBridgeState s;
    return s;
}

// 构造带真实 hooks 的实时管线：默认装配（manifest 校验 / SCRFD / ArcFace / 自检 / 基准），
// 仅覆盖 loadGallery —— 返回进程内已注册的不可变图库快照（空快照 = 合法空图库），
// 使 reload 产出的快照图库与 NativeFaceBridge.replaceGallery 的内存注册一致。
// galleryDir 参数保留契约位：当前图库来自内存注册，不读磁盘，故有意忽略。
rk_core::RealtimePipelineHooks makeActivatedHooks(const std::shared_ptr<GallerySlot>& slot) {
    rk_core::RealtimePipelineHooks hooks = rk_core::makeDefaultRealtimePipelineHooks();
    hooks.loadGallery =
        [slot](const rk_core::FaceRuntimeProfile&, const std::string&,
               std::string& err) -> std::shared_ptr<const FaceSearchLinearIndex> {
        err.clear();
        std::lock_guard<std::mutex> lock(slot->mutex);
        return slot->snapshot;
    };
    return hooks;
}

// 把当前图库快照注入 Engine 的图库覆盖接缝（Engine 未创建时为空操作）。
void injectGalleryToEngine(const std::shared_ptr<GallerySlot>& slot) {
    if (!g_engine) return;
    std::shared_ptr<const FaceSearchLinearIndex> snap;
    {
        std::lock_guard<std::mutex> lock(slot->mutex);
        snap = slot->snapshot;
    }
    g_engine->updateRealtimeGallery(std::move(snap));
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
/**
 * 校验并暂存 profile（语义澄清，P1-2）：
 * parseV1 只做格式/ABI/路径/SHA 字段格式校验，**不加载推理管线、不做模型文件存在性校验、
 * 不跑自检**。因此本方法只返回 validated/staged（effective 恒为 false），绝不谎报「已加载生效」。
 *
 * 「加载并生效」的唯一入口是 bridge_activateRuntime；文件级校验由两条正式路径承担：
 *   - Java FaceModelManager.verifyModelFiles（导入/切换前对稳定目录的存在性 + LFS + SHA-256）；
 *   - activateRuntime → RealtimeFacePipeline::reload → hooks.verifyFiles
 *     （= FaceRuntimeManifest::verifyFiles，对真实 baseDir 复核）。
 * 之所以不给本方法加 baseDir：会与上述两条路径重复，且扩大 JNI 面而不增加安全网。
 */
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
        // 仅置「已校验暂存」位；不放行 effective，避免被解释为管线已生效。
        s.profileValidated = true;
        // 仅记录 profileId/modelVersion 等非敏感元数据。
        rklog::logInfo(TAG, __func__, "face runtime profile validated (staged)");

        return toJString(env, okJson(
            "\"validated\":true,\"staged\":true,\"effective\":false"
            ",\"profileId\":\"" + jsonEscape(s.profile.profileId) +
            "\",\"modelVersion\":\"" + jsonEscape(s.profile.modelVersion) +
            "\",\"embeddingDim\":" + std::to_string(s.profile.embeddingDim)));
    } catch (const std::exception& e) {
        return toJString(env, errJson("FACE_PROFILE_EXCEPTION", e.what()));
    } catch (...) {
        return toJString(env, errJson("FACE_PROFILE_UNKNOWN", "unknown error"));
    }
}

// ========== activateRuntime ==========
/**
 * 激活实时管线：把 (manifest, baseDir, abi, galleryDir) 绑定到 Engine 的
 * RealtimeFacePipeline，使 Android 端从「仅预览 missing_model」切换到真实检测/识别。
 *
 * 两条路径（详见任务修复需求）：
 * - Engine 已创建：立即 setRealtimePipeline(带真实 hooks 的新管线) + reload；
 * - Engine 未创建：把参数暂存 FaceBridgeState（pendingActivation），待 jni/engine.cpp
 *   创建 Engine 后调用 applyPendingFaceRuntimeActivation() 补一次激活。
 *
 * 返回 JSON 仅含状态元数据（pending/effective/reason/generation），不含模板/向量。
 */
jstring bridge_activateRuntime(JNIEnv* env, jclass /* clazz */, jstring manifestJson,
                              jstring baseDir, jstring currentAbi, jstring galleryDir) {
    try {
        const std::string manifest = readString(env, manifestJson);
        const std::string base = readString(env, baseDir);
        const std::string abi = readString(env, currentAbi);
        const std::string gallery = readString(env, galleryDir);
        if (manifest.empty()) {
            return toJString(env, errJson("FACE_PROFILE_EMPTY", "manifest 为空"));
        }
        if (abi.empty()) {
            return toJString(env, errJson("FACE_PROFILE_ABI_EMPTY", "currentAbi 为空"));
        }
        if (base.empty()) {
            return toJString(env, errJson("FACE_RUNTIME_BASEDIR_EMPTY", "baseDir 为空"));
        }

        // 复用与 loadProfile 相同的严格 manifest 校验（ABI/路径/SHA 格式）。
        rk_core::FaceRuntimeProfile profile;
        std::string perr;
        if (!rk_core::FaceRuntimeManifest::parseV1(manifest, abi, profile, perr)) {
            return toJString(env, errJson("FACE_PROFILE_INVALID", perr));
        }

        auto& s = state();
        std::shared_ptr<GallerySlot> slot;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.profile = profile;
            // 校验暂存位；是否真正生效仍以 reload.ok（下文的 effective）为准。
            s.profileValidated = true;
            // 先登记 pending：若 Engine 尚未创建，engine.cpp 创建后据此补激活。
            s.pendingActivation = true;
            s.pendingManifest = manifest;
            s.pendingBaseDir = base;
            s.pendingAbi = abi;
            s.pendingGalleryDir = gallery;
            slot = s.gallerySlot;
        }

        auto pipe = std::make_shared<rk_core::RealtimeFacePipeline>(makeActivatedHooks(slot));

        bool pending = false;
        rk_core::RealtimeReloadResult reload;  // 默认 ok=false/reason 空/generation=0
        if (g_engine) {
            g_engine->setRealtimePipeline(pipe);
            reload = pipe->reload(manifest, base, abi, gallery);
            {
                // Engine 已存在：本次已直接尝试激活，无论成败都消费 pending，
                // 避免残留标记在 Engine 重建（若未来发生）时重复激活。
                std::lock_guard<std::mutex> lock(s.mutex);
                s.pendingActivation = false;
            }
            injectGalleryToEngine(slot);
        } else {
            pending = true;  // 不在此创建 Engine：由 nativeInit 负责，创建后补激活
        }

        // 仅记录状态/契约 reason，不含任何模型内容或向量。
        rklog::logInfo(TAG, __func__, std::string("realtime activation ") +
                                            (pending ? "pending" : (reload.ok ? "ok" : "failed")) +
                                            " reason=" + (pending ? std::string("pending") : reload.reason));

        return toJString(env, okJson(
            std::string("\"pending\":") + boolJson(pending) +
            ",\"effective\":" + boolJson(!pending && reload.ok) +
            ",\"reason\":\"" +
            jsonEscape(pending ? std::string("pending") : reload.reason) + "\"" +
            ",\"generation\":" + std::to_string(pending ? 0ULL : reload.generation)));
    } catch (const std::exception& e) {
        return toJString(env, errJson("FACE_RUNTIME_ACTIVATE_EXCEPTION", e.what()));
    } catch (...) {
        return toJString(env, errJson("FACE_RUNTIME_ACTIVATE_UNKNOWN", "unknown error"));
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
        std::size_t count = 0;
        std::size_t dim = 0;
        std::shared_ptr<const FaceSearchLinearIndex> gallerySnap;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            // reset 仅在全部校验通过后替换索引；失败时保留旧库。
            if (!s.gallery.reset(std::move(entries), kEmbeddingDim, indexErr)) {
                return toJString(env, errJson("FACE_GALLERY_INVALID", indexErr));
            }
            s.galleryLoaded = true;
            count = s.gallery.size();
            dim = s.gallery.dim();
            // 生成不可变快照供实时管线消费；模板/向量仅驻内存，绝不写入日志。
            gallerySnap = std::make_shared<const FaceSearchLinearIndex>(s.gallery);
            {
                std::lock_guard<std::mutex> slotLock(s.gallerySlot->mutex);
                s.gallerySlot->snapshot = gallerySnap;
            }
            rklog::logInfo(TAG, __func__, "gallery replaced count=" + std::to_string(count));
        }
        // 图库变化后同步到 Engine 的实时匹配接缝（Engine 未创建时忽略；激活时会重取快照）。
        if (g_engine) {
            g_engine->updateRealtimeGallery(gallerySnap);
        }

        return toJString(env, okJson(
            "\"count\":" + std::to_string(count) +
            ",\"dim\":" + std::to_string(dim)));
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
        // Engine 侧实时快照状态（不持 FaceBridgeState 锁，避免与 Engine 线程潜在锁序问题）。
        // effective：模型/自检/基准全部通过；reason：最近一次实时路径契约码。
        bool effective = false;
        std::string reason;
        if (g_engine) {
            effective = g_engine->getRealtimeEffective();
            reason = g_engine->getRealtimeReason();
        }
        // 尚未跑帧时 Engine reason 为空：按快照是否生效给出确定值，UI 不再误报就绪。
        if (reason.empty()) {
            reason = effective ? "ok" : "missing_model";
        }

        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const std::size_t galleryCount = s.galleryLoaded ? s.gallery.size() : 0;
        const std::size_t galleryDim = s.galleryLoaded ? s.gallery.dim() : 0;
        return toJString(env, okJson(
            // profileValidated：manifest 已校验并暂存（磁盘/格式层面），**不代表**管线已生效；
            // profileLoaded：仅为旧消费方（MainActivity 兜底）保留的同值别名，语义同 validated。
            // 是否「已加载生效」只看 effective/reason（来自 Engine 实时快照）。
            std::string("\"profileValidated\":") + boolJson(s.profileValidated) +
            ",\"profileLoaded\":" + boolJson(s.profileValidated) +
            ",\"profileId\":\"" + jsonEscape(s.profileValidated ? s.profile.profileId : std::string()) + "\"" +
            ",\"embeddingDim\":" + std::to_string(s.profileValidated ? s.profile.embeddingDim : 0) +
            ",\"galleryLoaded\":" + boolJson(s.galleryLoaded) +
            ",\"galleryCount\":" + std::to_string(galleryCount) +
            ",\"galleryDim\":" + std::to_string(galleryDim) +
            ",\"enrollmentState\":\"" + (s.enrollActive ? "ENROLLING" : "IDLE") + "\"" +
            ",\"effective\":" + boolJson(effective) +
            ",\"reason\":\"" + jsonEscape(reason) + "\""));
    } catch (const std::exception& e) {
        return toJString(env, errJson("FACE_RUNTIME_STATUS_EXCEPTION", e.what()));
    } catch (...) {
        return toJString(env, errJson("FACE_RUNTIME_STATUS_UNKNOWN", "unknown error"));
    }
}

}  // anonymous namespace

void applyPendingFaceRuntimeActivation() {
    // 仅在 Engine 已创建时由 jni/engine.cpp 调用；一次性消费 pending 参数。
    std::string manifest;
    std::string base;
    std::string abi;
    std::string gallery;
    std::shared_ptr<GallerySlot> slot;
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.pendingActivation) return;
        manifest = s.pendingManifest;
        base = s.pendingBaseDir;
        abi = s.pendingAbi;
        gallery = s.pendingGalleryDir;
        slot = s.gallerySlot;
        s.pendingActivation = false;
    }
    if (!g_engine || manifest.empty() || base.empty() || abi.empty()) return;

    auto pipe = std::make_shared<rk_core::RealtimeFacePipeline>(makeActivatedHooks(slot));
    g_engine->setRealtimePipeline(pipe);
    const rk_core::RealtimeReloadResult r = pipe->reload(manifest, base, abi, gallery);
    if (!r.ok) {
        // 待激活补做失败：显式记录契约 reason（模型缺失等），由 getRuntimeStatus 暴露。
        rklog::logWarn(TAG, __func__, "pending activation failed reason=" + r.reason);
    }
    injectGalleryToEngine(slot);
}

void registerFaceBridgeMethods(JniMethodRegistry& registry) {
    registry.add({
        {"loadProfile",
         "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
         reinterpret_cast<void*>(bridge_loadProfile)},
        {"activateRuntime",
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
         reinterpret_cast<void*>(bridge_activateRuntime)},
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
