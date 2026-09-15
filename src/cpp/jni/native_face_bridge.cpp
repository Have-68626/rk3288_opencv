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
#include <limits>
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

// #1 注册采集回调的 metrics 打包数组长度（须与 Java 侧 NativeFaceBridge.ENROLL_METRICS_LEN 一致）：
//   [0]frameWidth [1]frameHeight [2]detScore [3]faceCount
//   [4]grayMean [5]grayStd [6]laplacianVariance [7]areaRatio
//   [8..11]bbox(left,top,right,bottom) [12..21]landmarks(x0,y0,...,x4,y4)
constexpr jsize kEnrollMetricsLen = 22;

// #4 每线程 JNIEnv 缓存：注册采集回调始终运行在同一 native 引擎线程，逐脸
// AttachCurrentThread/DetachCurrentThread 的开销在多人脸/30fps 下可观；改为本线程首次
// 回调时 Attach 一次并复用，线程退出时由缓存对象析构统一 Detach（仅 Detach 本线程
// 自己 Attach 的情形；Java 线程自带 env 不 Detach）。
struct ThreadEnvCache {
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    bool attachedByUs = false;
    ~ThreadEnvCache() {
        if (attachedByUs && vm) vm->DetachCurrentThread();
    }
};

/**
 * 取本线程可用的 JNIEnv，必要时 Attach（仅首次）。
 * @return 可用 env；GetEnv 返回 JNI_EVERSION 等非 JNI_OK/JNI_EDETACHED 错误时返回 nullptr，
 *         调用方必须兜底早退，否则会在未初始化的 env 上解引用（NewFloatArray 等）崩溃。
 */
JNIEnv* acquireThreadEnv(JavaVM* vm) {
    static thread_local ThreadEnvCache cache;
    if (vm == nullptr) return nullptr;
    if (cache.env != nullptr && cache.vm == vm) return cache.env;
    JNIEnv* env = nullptr;
    const jint st = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) return nullptr;
        cache.attachedByUs = true;
    } else if (st != JNI_OK || env == nullptr) {
        return nullptr;  // 非 EDETACHED 的其他错误码：env 未初始化，直接放弃本帧
    } else {
        cache.attachedByUs = false;  // env 由 Java 线程自带，线程退出时不 Detach
    }
    cache.vm = vm;
    cache.env = env;
    return env;
}

// 标记本线程正身处 Java 注册采集回调内。若 Java 监听器在回调内同步调用
// start/stopEnrollmentCapture，JNI 边界据此显式拒绝（返回错误 JSON），避免其与正在等待
// 在途回调的关闭路径争用生命周期锁形成死锁；文档见 NativeFaceBridge 对应方法。
thread_local bool t_inEnrollmentFrameCallback = false;

/**
 * #1 注册采集回调槽：持有 Java 侧 direct ByteBuffer 的全局引用与回调方法 ID，
 * 供 native 引擎线程逐帧回调。字段读写均在 mutex 下进行。
 *
 * 生命周期由 startEnrollmentCapture/stopEnrollmentCapture 管理：
 * stop 先经 Engine::setEnrollmentCapture(false) 静默在途回调，再释放全局引用。
 */
struct EnrollmentCaptureSlot {
    // 串行化 start/stop 的整段状态变更。slot 字段与 Engine 的 enabled/cb 分属两处，
    // 若 start 的「置 active=true → setEnrollmentCapture(true)」被并发 stop 穿插，会出现
    // enabled=true 而 active=false/cb_=nullptr 的谎报；持本锁可消除该穿插。
    // 注意：不可在持本锁时等待 Engine 在途回调（否则回调取 slot.mutex 时互等死锁）。
    std::mutex lifecycleMutex;
    std::mutex mutex;
    bool active = false;
    jobject embeddingBuffer = nullptr;  // direct ByteBuffer 全局引用（内嵌回传 512 维）
    void* embeddingAddr = nullptr;      // 上述 ByteBuffer 的底层地址（回调内 memcpy 写入）
    jclass bridgeClass = nullptr;       // NativeFaceBridge 全局引用（跨线程调用必需）
    jmethodID onFrameMethod = nullptr;  // onEnrollmentFrame(ByteBuffer, float[])
    JavaVM* vm = nullptr;               // 回调线程 Attach 用
};

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

    // #1 注册采集回调槽（shared_ptr 仅初始化一次，不重置，故回调线程可无锁读取引用）。
    std::shared_ptr<EnrollmentCaptureSlot> captureSlot = std::make_shared<EnrollmentCaptureSlot>();

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
//
// 语义修正（上轮未覆盖项 B）：改用**原子提交** applyGalleryUpdate（换库 + 按新库有效身份
// 精准撤销身份），取代只换库、不撤销身份的 updateRealtimeGallery。原因：activateRuntime 换
// 模型后，旧模型轨迹的 stableId 若不撤销会被持续保留——而新旧模型嵌入空间不同，残留 id 在新库
// 并无对应，表现为「换模型后持续 VERIFIED <旧id>」直至该人离开画面 2 帧。keepIds 取新库全部
// personId（FaceSearchLinearIndex::entries() 的 e.id，与 bridge_replaceGallery 同法），未命中者
// 身份清为 "Unknown"（保留轨迹本体与 bbox）。
void injectGalleryToEngine(const std::shared_ptr<GallerySlot>& slot) {
    if (!g_engine) return;
    std::shared_ptr<const FaceSearchLinearIndex> snap;
    {
        std::lock_guard<std::mutex> lock(slot->mutex);
        snap = slot->snapshot;
    }
    // 由新图库快照构造有效身份集合；空快照 → 空 keepIds → 全部撤销（不丢框）。
    std::vector<std::string> keepIds;
    if (snap) {
        const std::vector<FaceSearchEntry>& entries = snap->entries();
        keepIds.reserve(entries.size());
        for (const FaceSearchEntry& e : entries) keepIds.push_back(e.id);
    }
    g_engine->applyGalleryUpdate(std::move(snap), keepIds);
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
 * - Engine 已创建：先让候选管线 reload，**成功才** setRealtimePipeline(新管线)；
 *   失败不替换，旧管线与 Engine 的 realtimeGallery_ 原样保留（#2 修复）。
 * - Engine 未创建：把参数暂存 FaceBridgeState（pendingActivation），待 jni/engine.cpp
 *   创建 Engine 后调用 applyPendingFaceRuntimeActivation() 补一次激活。
 *
 * 返回 JSON 仅含状态元数据（pending/effective/reason/generation），不含模板/向量。
 * reload 失败时返回 ok=false + reason/evidence，供 Java 侧回滚激活指针并保留旧配置。
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
            // 先登记 pending 参数：若 Engine 尚未创建，engine.cpp 创建后据此补激活；
            // Engine 已存在且本次 reload 成功提交后才消费（见下）。激活失败保留该标记，
            // 既允许后续重试，也绝不制造「已生效」假象。
            s.pendingActivation = true;
            s.pendingManifest = manifest;
            s.pendingBaseDir = base;
            s.pendingAbi = abi;
            s.pendingGalleryDir = gallery;
            slot = s.gallerySlot;
        }

        bool pending = false;
        rk_core::RealtimeReloadResult reload;  // 默认 ok=false/reason 空/generation=0
        if (g_engine) {
            // #2 修复：候选管线先在 Engine 之外完成 reload，成功才提交替换。
            // 旧实现先 setRealtimePipeline（替换旧管线并清空 realtimeGallery_）再 reload，
            // reload 失败无法回滚 -> 旧配置被丢弃且仍报成功。此处 reload 失败时旧管线与
            // realtimeGallery_ 原样保留，绝不提交半成品。
            auto pipe = std::make_shared<rk_core::RealtimeFacePipeline>(makeActivatedHooks(slot));
            reload = pipe->reload(manifest, base, abi, gallery);
            if (reload.ok) {
                g_engine->setRealtimePipeline(pipe);
                injectGalleryToEngine(slot);
                // 提交成功才消费 pending，避免 Engine 重建（若未来发生）时重复激活。
                std::lock_guard<std::mutex> lock(s.mutex);
                s.pendingActivation = false;
            }
            // reload 失败：不替换旧管线、不消费 pending（保留参数以便重试）；
            // effective 仍由旧快照决定，不会留下「已生效」假象。
        } else {
            pending = true;  // 不在此创建 Engine：由 nativeInit 负责，创建后补激活
        }

        if (pending) {
            // pending 属正常时序（Engine 尚未创建），effective 恒 false。
            rklog::logInfo(TAG, __func__, "realtime activation pending");
            return toJString(env, okJson(
                "\"pending\":true,\"effective\":false,\"reason\":\"pending\",\"generation\":0"));
        }
        if (!reload.ok) {
            // 激活失败：ok=false + reason/evidence；Java 侧据此回滚指针并保留旧配置。
            rklog::logWarn(TAG, __func__, "realtime activation failed reason=" + reload.reason);
            return toJString(env,
                std::string("{\"ok\":false,\"pending\":false,\"effective\":false,\"reason\":\"") +
                jsonEscape(reload.reason) + "\",\"generation\":" +
                std::to_string(reload.generation) +
                ",\"error\":{\"code\":\"FACE_RUNTIME_ACTIVATE_FAILED\",\"message\":\"" +
                jsonEscape(reload.evidence.empty() ? reload.reason : reload.evidence) +
                "\"}}");
        }
        // 仅记录状态/契约 reason，不含任何模型内容或向量。
        rklog::logInfo(TAG, __func__, "realtime activation ok reason=" + reload.reason);
        return toJString(env, okJson(
            "\"pending\":false,\"effective\":true,\"reason\":\"ok\",\"generation\":" +
            std::to_string(reload.generation)));
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
        // 本次图库有效身份集合（personIds），供跟踪身份**精准撤销**（第三波审查 #1：
        // 只撤销已不在新图库中的身份，保留其他人的稳定身份）。仅作内存内临时集合，
        // 绝不写入日志（安全红线）。
        std::vector<std::string> keepIds;
        keepIds.reserve(static_cast<std::size_t>(idCount));
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
            // 先复制 id 进有效身份集合，再随 entry 移入图库（entry.id 之后被 reset 移走）。
            keepIds.push_back(entry.id);
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
            // 删除人员 / 注册提交均经本 replaceGallery 路径。**原子提交（第三波审查 #4 残留竞态
            // 闭合）**：改由 applyGalleryUpdate 在**单一 tracksMu_ 临界区**内一次性完成
            //「换图库覆盖 + 按新库有效身份 keepIds 精准撤销跟踪身份」。
            // 因果：此前是 resetTrackIdentitiesExcept(keepIds) 与 updateRealtimeGallery(snap)
            // **两次独立持锁调用**，二者之间的缝隙内引擎线程可用**旧图库**跑完一帧——该帧检测
            // 已认证（旧库含被删者）会把身份重新写入轨迹，随后新库（已无此人）生效却被持久保留，
            // 等同 #2 复发。合并为单次调用后，「换库」与「撤销」之间不存在可观测中间态：任何
            // 引擎帧要么看到（旧库 + 旧身份），要么看到（新库 + 已撤销身份）。
            // 精准撤销而非全清：多人场景删除/注册一人不再抹掉其他人的稳定身份（第三波审查 #1）；
            // 空图库 → keepIds 为空 → 全部清为 Unknown（不丢框）。
            // 注：Engine 侧 assignRealtimeIdentities 读库走 realtimeGalleryMu_（非 tracksMu_），
            // 故另有「读库→提交」更小窗口，已由 Engine 的图库代次作废机制闭合（见 applyGalleryUpdate）。
            g_engine->applyGalleryUpdate(gallerySnap, keepIds);
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

// ========== #1 注册采集逐帧回调 ==========

/**
 * Engine 注册采集回调（由 run()/processFrameOnce 线程调用）。
 *
 * 数据流：Engine 逐脸产出 EnrollmentCaptureSample → 本函数把 512 维向量 memcpy 进 Java
 * 传入的 direct ByteBuffer（**绝不**经字符串/日志），再把几何/质量指标打包为 float[]，
 * 调用 NativeFaceBridge.onEnrollmentFrame(ByteBuffer, float[])。
 *
 * 线程：本函数运行在 native 引擎线程，非 JVM 线程。env 经 acquireThreadEnv 每线程只
 * Attach 一次并复用（不再逐脸 Attach/Detach），线程退出时由 ThreadEnvCache 统一 Detach。
 */
void onEnrollmentCaptureSample(const rk_core::EnrollmentCaptureSample& s) {
    // captureSlot 初始化后不再替换，引用稳定；不持 state().mutex 以免与激活路径争用。
    EnrollmentCaptureSlot& slot = *state().captureSlot;

    jobject bufGlobal = nullptr;
    jclass clsGlobal = nullptr;
    jmethodID mid = nullptr;
    void* addr = nullptr;
    JavaVM* vm = nullptr;
    {
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (!slot.active) return;
        bufGlobal = slot.embeddingBuffer;
        clsGlobal = slot.bridgeClass;
        mid = slot.onFrameMethod;
        addr = slot.embeddingAddr;
        vm = slot.vm;
    }
    if (!bufGlobal || !clsGlobal || !mid || !addr || !vm) return;
    if (s.embedding.size() != kEmbeddingDim) return;

    // #2/#4 先取本线程 env（每线程仅 Attach 一次并复用）。GetEnv 非 EDETACHED 失败时
    // 返回 nullptr，必须兜底早退：否则后续 NewFloatArray 会在未初始化的 env 上解引用崩溃。
    JNIEnv* env = acquireThreadEnv(vm);
    if (env == nullptr) return;

    // 向量只写入 direct ByteBuffer，不进入任何字符串/日志（安全红线）。放在 env 就绪之后，
    // 避免投递失败时在复用缓冲区里残留本帧模板。
    std::memcpy(addr, s.embedding.data(), kEmbeddingBytes);

    // 无关键点时以 NaN 占位，Java 侧据此构造 landmarks=null（姿态判不定 → 拒收该帧）。
    const jfloat nan = std::numeric_limits<jfloat>::quiet_NaN();
    jfloat metrics[kEnrollMetricsLen];
    metrics[0] = static_cast<jfloat>(s.frameWidth);
    metrics[1] = static_cast<jfloat>(s.frameHeight);
    metrics[2] = static_cast<jfloat>(s.detScore);
    metrics[3] = static_cast<jfloat>(s.faceCount);
    metrics[4] = static_cast<jfloat>(s.grayMean);
    metrics[5] = static_cast<jfloat>(s.grayStd);
    metrics[6] = static_cast<jfloat>(s.laplacianVariance);
    metrics[7] = static_cast<jfloat>(s.areaRatio);
    metrics[8] = static_cast<jfloat>(s.bbox.x);
    metrics[9] = static_cast<jfloat>(s.bbox.y);
    metrics[10] = static_cast<jfloat>(s.bbox.x + s.bbox.width);
    metrics[11] = static_cast<jfloat>(s.bbox.y + s.bbox.height);
    for (int i = 0; i < 10; ++i) {
        metrics[12 + i] = s.hasLandmarks ? static_cast<jfloat>(s.landmarks[i]) : nan;
    }

    jfloatArray arr = env->NewFloatArray(kEnrollMetricsLen);
    if (arr != nullptr) {
        env->SetFloatArrayRegion(arr, 0, kEnrollMetricsLen, metrics);
        // 标记本线程正身处回调内：Java 监听器若同步调用 start/stop 会被 JNI 边界拒绝，
        // 避免与等待在途回调的关闭路径争用生命周期锁死锁。
        t_inEnrollmentFrameCallback = true;
        // 真实向量不入参该方法（仅缓冲区引用），签名不含任何模板字符串。
        env->CallStaticVoidMethod(clsGlobal, mid, bufGlobal, arr);
        t_inEnrollmentFrameCallback = false;
        if (env->ExceptionCheck()) {
            // 回调异常不得穿透 native 边界：清除并放弃本帧样本，不记录向量内容。
            env->ExceptionClear();
        }
        env->DeleteLocalRef(arr);
    }
}

// ========== startEnrollmentCapture ==========
/**
 * 开启注册采集：注册 Engine 回调并启用逐帧采集模式。
 *
 * 重入约束：不得在 onEnrollmentFrame 回调内同步调用本方法，否则返回
 * {@code FACE_ENROLL_CAPTURE_REENTRANT}（见 stopEnrollmentCapture 说明）。
 *
 * @param embeddingBuffer Java 侧复用的 direct ByteBuffer（精确 512*4 字节，LE float32），
 *                        native 逐帧写入后经 onEnrollmentFrame 回传；不分配新缓冲区。
 * @return 成功 {"ok":true,"capturing":true,...}；失败返回错误 JSON（含 error.code）
 */
jstring bridge_startEnrollmentCapture(JNIEnv* env, jclass /* clazz */, jobject embeddingBuffer) {
    try {
        if (t_inEnrollmentFrameCallback) {
            return toJString(env, errJson("FACE_ENROLL_CAPTURE_REENTRANT",
                "禁止在注册采集回调内同步调用 startEnrollmentCapture"));
        }
        if (embeddingBuffer == nullptr) {
            return toJString(env, errJson("FACE_ENROLL_CAPTURE_NULL", "embedding 缓冲区为空"));
        }
        void* addr = env->GetDirectBufferAddress(embeddingBuffer);
        const jlong capacity = env->GetDirectBufferCapacity(embeddingBuffer);
        if (addr == nullptr || capacity < 0) {
            return toJString(env, errJson("FACE_ENROLL_CAPTURE_NOT_DIRECT",
                                          "embedding 必须为 direct ByteBuffer"));
        }
        if (capacity != static_cast<jlong>(kEmbeddingBytes)) {
            return toJString(env, errJson("FACE_ENROLL_CAPTURE_BAD_LENGTH",
                "embedding 长度必须精确为 " + std::to_string(kEmbeddingBytes) + " 字节"));
        }
        if (!g_engine) {
            return toJString(env, errJson("FACE_ENROLL_CAPTURE_NO_ENGINE", "引擎尚未创建"));
        }

        // #3 串行化 start/stop 的整段状态变更：防止并发 stop 穿插在「置 slot.active=true」
        // 与「setEnrollmentCapture(true)」之间，造成 enabled=true 而 active=false/cb_=nullptr
        // 的谎报（isEnrollmentCapture 失真）。本锁内不得等待 Engine 在途回调。
        auto& s = state();
        EnrollmentCaptureSlot& slot = *s.captureSlot;
        std::lock_guard<std::mutex> lifeLock(slot.lifecycleMutex);

        // 先静默旧采集（若存在）：setEnrollmentCapture(false) 返回即无跨线程在途回调，
        // 之后才能安全释放旧缓冲区/类的全局引用。重复 start 幂等。
        g_engine->setEnrollmentCapture(false);

        jobject oldBuf = nullptr;
        jclass oldCls = nullptr;
        {
            std::lock_guard<std::mutex> lock(slot.mutex);
            slot.active = false;
            oldBuf = slot.embeddingBuffer;
            oldCls = slot.bridgeClass;
            slot.embeddingBuffer = nullptr;
            slot.bridgeClass = nullptr;
            slot.embeddingAddr = nullptr;
            slot.onFrameMethod = nullptr;
        }
        if (oldBuf) env->DeleteGlobalRef(oldBuf);
        if (oldCls) env->DeleteGlobalRef(oldCls);

        jclass localCls = env->FindClass("com/example/rk3288_opencv/NativeFaceBridge");
        if (!localCls) {
            env->ExceptionClear();
            return toJString(env, errJson("FACE_ENROLL_CAPTURE_NO_CLASS", "找不到 NativeFaceBridge"));
        }
        jmethodID mid = env->GetStaticMethodID(
            localCls, "onEnrollmentFrame", "(Ljava/nio/ByteBuffer;[F)V");
        if (!mid) {
            env->ExceptionClear();
            env->DeleteLocalRef(localCls);
            return toJString(env, errJson("FACE_ENROLL_CAPTURE_NO_CALLBACK",
                                          "缺少 onEnrollmentFrame 回调声明"));
        }
        jobject bufGlobal = env->NewGlobalRef(embeddingBuffer);
        jclass clsGlobal = static_cast<jclass>(env->NewGlobalRef(localCls));
        env->DeleteLocalRef(localCls);
        if (!bufGlobal || !clsGlobal) {
            if (bufGlobal) env->DeleteGlobalRef(bufGlobal);
            if (clsGlobal) env->DeleteGlobalRef(clsGlobal);
            env->ExceptionClear();
            return toJString(env, errJson("FACE_ENROLL_CAPTURE_REF_FAILED", "全局引用创建失败"));
        }

        // 先注册回调，再置 active，最后开启 Engine 采集——保证首个样本回调时槽已就绪。
        g_engine->setEnrollmentCaptureCallback(onEnrollmentCaptureSample);
        {
            std::lock_guard<std::mutex> lock(slot.mutex);
            slot.embeddingBuffer = bufGlobal;
            slot.bridgeClass = clsGlobal;
            slot.embeddingAddr = addr;
            slot.onFrameMethod = mid;
            slot.vm = g_vm;
            slot.active = true;
        }
        g_engine->setEnrollmentCapture(true);
        rklog::logInfo(TAG, __func__, "enrollment capture started");
        return toJString(env, okJson("\"capturing\":true"));
    } catch (const std::exception& e) {
        return toJString(env, errJson("FACE_ENROLL_CAPTURE_EXCEPTION", e.what()));
    } catch (...) {
        return toJString(env, errJson("FACE_ENROLL_CAPTURE_UNKNOWN", "unknown error"));
    }
}

// ========== stopEnrollmentCapture ==========
/**
 * 停止注册采集：先经 Engine 静默在途回调（跨线程 setEnrollmentCapture(false) 返回即无
 * 在途回调），再释放 direct ByteBuffer / 类的全局引用。
 *
 * 重入约束：不得在 onEnrollmentFrame 回调内同步调用本方法——此时返回
 * {@code FACE_ENROLL_CAPTURE_REENTRANT}。原因：关闭需等待在途回调归零，而回调正身处
 * 该线程，自等必死锁；若另一线程已在关闭中持生命周期锁，重入还会与在途回调互等。
 * 若确需在回调中停止采集，请切换到其他线程（如 post 到 UI 线程）后再调用。
 */
jstring bridge_stopEnrollmentCapture(JNIEnv* env, jclass /* clazz */) {
    try {
        if (t_inEnrollmentFrameCallback) {
            return toJString(env, errJson("FACE_ENROLL_CAPTURE_REENTRANT",
                "禁止在注册采集回调内同步调用 stopEnrollmentCapture"));
        }
        auto& s = state();
        EnrollmentCaptureSlot& slot = *s.captureSlot;
        // #3 与 start 共用生命周期锁，消除两者状态变更穿插。
        std::lock_guard<std::mutex> lifeLock(slot.lifecycleMutex);

        // 静默：Engine 关闭会等待在途回调返回，故返回后回调不再触达下方将被释放的引用。
        if (g_engine) {
            g_engine->setEnrollmentCapture(false);
            g_engine->setEnrollmentCaptureCallback(nullptr);
        }
        jobject buf = nullptr;
        jclass cls = nullptr;
        {
            std::lock_guard<std::mutex> lock(slot.mutex);
            slot.active = false;
            buf = slot.embeddingBuffer;
            cls = slot.bridgeClass;
            slot.embeddingBuffer = nullptr;
            slot.bridgeClass = nullptr;
            slot.embeddingAddr = nullptr;
            slot.onFrameMethod = nullptr;
        }
        if (buf) env->DeleteGlobalRef(buf);
        if (cls) env->DeleteGlobalRef(cls);
        rklog::logInfo(TAG, __func__, "enrollment capture stopped");
        return toJString(env, okJson("\"capturing\":false"));
    } catch (const std::exception& e) {
        return toJString(env, errJson("FACE_ENROLL_CAPTURE_STOP_EXCEPTION", e.what()));
    } catch (...) {
        return toJString(env, errJson("FACE_ENROLL_CAPTURE_STOP_UNKNOWN", "unknown error"));
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
        // 有意不在此处清 pending：只有候选管线 reload 成功提交后才消费，
        // 失败则保留标记（不制造「已生效」假象，且 Engine 重建时可重试）。
    }
    if (!g_engine || manifest.empty() || base.empty() || abi.empty()) return;

    // #2 修复：与 bridge_activateRuntime 同序——候选管线先 reload，成功才提交替换；
    // 失败保留 Engine 现有管线与 realtimeGallery_，绝不先替换再 reload。
    auto pipe = std::make_shared<rk_core::RealtimeFacePipeline>(makeActivatedHooks(slot));
    const rk_core::RealtimeReloadResult r = pipe->reload(manifest, base, abi, gallery);
    if (!r.ok) {
        // 待激活补做失败：显式记录契约 reason（模型缺失等），由 getRuntimeStatus 暴露。
        rklog::logWarn(TAG, __func__, "pending activation failed reason=" + r.reason);
        return;
    }
    g_engine->setRealtimePipeline(pipe);
    injectGalleryToEngine(slot);
    {
        // 提交成功才消费 pending。
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pendingActivation = false;
    }
    rklog::logInfo(TAG, __func__, "pending activation applied");
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
        {"startEnrollmentCapture",
         "(Ljava/nio/ByteBuffer;)Ljava/lang/String;",
         reinterpret_cast<void*>(bridge_startEnrollmentCapture)},
        {"stopEnrollmentCapture",
         "()Ljava/lang/String;",
         reinterpret_cast<void*>(bridge_stopEnrollmentCapture)},
        {"getRuntimeStatus",
         "()Ljava/lang/String;",
         reinterpret_cast<void*>(bridge_getRuntimeStatus)},
    });
}
