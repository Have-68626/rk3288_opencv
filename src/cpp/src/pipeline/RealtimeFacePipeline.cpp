#include "pipeline/RealtimeFacePipeline.h"

#include "AccelerationContract.h"
#include "FaceSearch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#if !defined(RK_SKIP_OPENCV)
#include "Embedder.h"
#include "FaceDetector.h"
#include "adapters/ArcFaceAdapter.h"
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
#include "adapters/ScrfdNcnnAdapter.h"
#endif
#endif

namespace rk_core {

namespace {

using Clock = std::chrono::steady_clock;

// 预算强制默认值：优先取构建期显式选项（RK_BENCH_ENFORCE_BUDGET），否则回落架构判定。
// 把 #if 从**头文件**挪到 TU 内部，是为了让 RealtimeBenchmarkConfig 的定义在所有 TU/所有
// 目标下逐字一致（ODR 安全）——编译期按目标预定义 __aarch64__ 本身是 ODR 安全的，但只要
// 该分支留在头文件里，一旦将来改成人工维护的显式宏就有静默 UB 的入口。
constexpr bool kDefaultEnforceBudget =
#if defined(RK_BENCH_ENFORCE_BUDGET)
    RK_BENCH_ENFORCE_BUDGET != 0;
#elif defined(__aarch64__)
    true;
#else
    false;
#endif

double durationMs(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// p95 分位：排序后取 ceil(p*n)-1（含端点的最近秩估计），n==0 返回 0。
double percentile95(std::vector<double> samples) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const std::size_t idx = static_cast<std::size_t>(
        std::ceil(0.95 * static_cast<double>(samples.size()))) - 1;
    return samples[std::min(idx, samples.size() - 1)];
}

std::string fmtMs(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << v;
    return oss.str();
}

// 构造填充为常量灰度的合成 BGR 图（不依赖 cv::Mat::setTo，兼容 SKIP_OPENCV mock）。
cv::Mat makeSyntheticBgr(int w, int h) {
    cv::Mat img(h > 0 ? h : 1, w > 0 ? w : 1, CV_8UC3);
    if (img.data && w > 0 && h > 0) {
        std::memset(img.data, 128, static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u);
    }
    return img;
}

RealtimeReloadResult failResult(const std::string& reason, const std::string& evidence,
                                std::uint64_t generation) {
    RealtimeReloadResult r;
    r.ok = false;
    r.reason = reason;
    r.evidence = evidence;
    r.generation = generation;
    return r;
}

// 把加载层 err 归一到契约 reason 码；未知文本一律 runtime_init_failed。
std::string reasonFromLoadError(const std::string& err) {
    return rk_accel::normalizeReasonCode(err, /*requested=*/true, /*effective=*/false);
}

// 把 verifyFiles 钩子的 err 归一到契约 reason 码。**不能**直接套 reasonFromLoadError：
// FaceRuntimeManifest::verifyFiles（src/cpp/src/FaceRuntimeProfile.cpp）写出的四类 err 全是
// **人类文本**（非法模型路径 / 缺失模型文件 / Git LFS pointer / SHA-256 不匹配），
// 没有任何一类能命中 normalizeReasonCode 的固定码表，会被**一律**归一成兜底码
// runtime_init_failed——即把「模型缺失」这类明确语义降级成「运行时初始化失败」，
// 反而比原先硬编码 missing_model 更难排障。
//
// 归一顺序：
//   1) 先认 verifyFiles 的稳定文案，按语义落到两个不同码：
//      · "缺失模型文件: <相对路径>"                    → missing_model（文件不在位）
//      · "非法模型路径（绝对或含 ..）：<相对路径>"      ┐
//        "模型文件是 Git LFS pointer（未拉取）: <...>"  ├→ unsupported_input
//        "SHA-256 不匹配: <相对路径>"                   ┘
//      后三类一律归 unsupported_input 而非 missing_model：它们出错在**调用方提供的
//      artifacts / manifest 自身**（路径非法、指向指针文件、内容与清单摘要不符），属输入
//      契约问题；missing_model 保留给「本环境确实缺该文件」这一种。
//   2) 都不是（自定义钩子按钩子契约直接写固定码，或写未知文本）→ 交 reasonFromLoadError
//      统一归一：固定码原样透传，未知文本归 runtime_init_failed。
//
// ⚠️ 已知脆弱点（如实记录，勿高估）：第 1 步用**中文字面量子串**匹配，FaceRuntimeProfile.cpp
//    里上述四句文案一旦改字，此处会**静默**退化为 `runtime_init_failed`（不报错、不崩溃、无告警）。
//    注意退化目标**不是** `unsupported_input`：四个分支的 find 全落空 ⇒ 交由 reasonFromLoadError →
//    normalizeReasonCode（AccelerationContract.h）把**未知文本**在 requested && effective==false 时
//    兜底成 `runtime_init_failed`；`unsupported_input` 只由上面「非法路径 / LFS pointer / SHA-256
//    不匹配」这类**实际命中**的文本给出，改字恰恰让它们**不再**命中。
//    彻底消除需给 FaceRuntimeManifest::verifyFiles 加一个 reasonCode 出参（须改
//    FaceRuntimeProfile.h，超出本批改动面），已登记为后续项。
std::string reasonFromVerifyError(const std::string& err) {
    if (err.find("缺失模型文件") != std::string::npos) return "missing_model";
    if (err.find("非法模型路径") != std::string::npos ||
        err.find("Git LFS pointer") != std::string::npos ||
        err.find("SHA-256 不匹配") != std::string::npos) {
        return "unsupported_input";
    }
    return reasonFromLoadError(err);
}

#if !defined(RK_SKIP_OPENCV)

// 桥接 rk_core::Embedder → IRealtimeEmbedder（生产识别句柄）。
class EmbedderRealtimeAdapter final : public IRealtimeEmbedder {
public:
    explicit EmbedderRealtimeAdapter(std::shared_ptr<Embedder> inner) : inner_(std::move(inner)) {}

    bool embed(const cv::Mat& alignedFaceBgr, std::vector<float>& out, std::string& err) override {
        std::optional<std::vector<float>> r = inner_->embed(alignedFaceBgr, err);
        if (!r.has_value()) {
            if (err.empty()) err = "runtime_init_failed";
            return false;
        }
        out = std::move(*r);
        return true;
    }
    int embeddingDim() const override { return inner_->embeddingDim(); }
    const char* name() const override { return inner_->name(); }

private:
    std::shared_ptr<Embedder> inner_;
};

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
// 桥接 ScrfdNcnnAdapter（FaceDetector）→ IRealtimeDetector（生产检测句柄）。
// RealtimeFaceBox 有意不复用 FaceDetection，避免头文件引入 cv::Point2f。
class ScrfdRealtimeAdapter final : public IRealtimeDetector {
public:
    explicit ScrfdRealtimeAdapter(std::shared_ptr<ScrfdNcnnAdapter> inner)
        : inner_(std::move(inner)) {}

    bool detect(const cv::Mat& bgr, std::vector<RealtimeFaceBox>& out, std::string& err) override {
        const FaceDetections faces = inner_->detect(bgr, err);
        if (!err.empty()) return false;
        out.clear();
        out.reserve(faces.size());
        for (const FaceDetection& f : faces) {
            RealtimeFaceBox b{f.bbox.x, f.bbox.y, f.bbox.width, f.bbox.height, f.score};
            // #1 注册采集：把检测关键点带出到统一框结构，供对齐与姿态分类使用。
            if (f.keypoints5.has_value()) {
                b.hasKeypoints = true;
                for (std::size_t i = 0; i < 5; ++i) {
                    b.keypoints[i * 2] = (*f.keypoints5)[i].x;
                    b.keypoints[i * 2 + 1] = (*f.keypoints5)[i].y;
                }
            }
            out.push_back(b);
        }
        return true;
    }
    const char* name() const override { return "scrfd_ncnn"; }

private:
    std::shared_ptr<ScrfdNcnnAdapter> inner_;
};
#endif  // RK_HAVE_NCNN

#endif  // !RK_SKIP_OPENCV

}  // namespace

// 生产默认钩子装配。可用于生产推理的三个钩子（verifyFiles / loadDetector / loadEmbedder）
// 都依赖真实模型与 ncnn：
//
// ⚠️ **CI 覆盖缺口（如实记录，勿误判为代码缺陷）**：本仓 CI 的两个 job **都不编译也不执行**
//    SCRFD / ArcFace 的真实装配接线，两个 load* 钩子在其中**恒走 `#else` 的 build_disabled
//    分支**：
//      · job `unit-tests` 传 `-DRK_SKIP_OPENCV=ON`（见 .github/workflows/ci.yml）⇒ 命中
//        `!defined(RK_SKIP_OPENCV)` 为假；
//      · job `windows` 的 CMake configure 只传 OPENCV_ROOT / OPENCV_CONTRIB_ROOT /
//        BUILD_LIST，**未传 `-DRK_ENABLE_NCNN=ON`**，而该选项默认 OFF（CMakeLists.txt）⇒
//        `RK_HAVE_NCNN=0` ⇒ 命中 `RK_HAVE_NCNN` 为假。
//    又因两个 `load*` 钩子只被本函数（默认构造函数）与 native_face_bridge.cpp 的生产装配
//    引用，**无任何测试调用默认钩子的这两个 lambda**（测试一律注入自定义钩子）。
//    ⇒ 「manifest → SCRFD/ArcFace 装配」这段真实接线在 CI 内**零覆盖**。
//    不打算在 CI 里打开 ncnn 来补（需拉 ncnn 且模型二进制不入库，成本高于收益），
//    此处仅标注盲区，供人工验证与后续评估。
RealtimePipelineHooks makeDefaultRealtimePipelineHooks() {
    RealtimePipelineHooks h;

    // manifest 文件级校验：路径安全（相对且无 ..）/ 存在 / 非 Git LFS pointer / SHA-256 匹配。
    h.verifyFiles = [](const FaceRuntimeProfile& profile, const std::string& baseDir,
                       std::string& err) {
        return FaceRuntimeManifest::verifyFiles(profile, baseDir, err);
    };

    // SCRFD 装配：复用 ScrfdNcnnAdapter::configFromProfile 把 manifest 解码语义
    // （节点名/decodeMode/anchorsPerPos/outputLayout/阈值）接入运行路径。
    h.loadDetector = [](const FaceRuntimeProfile& profile, const std::string& baseDir,
                        std::string& err) -> std::shared_ptr<IRealtimeDetector> {
#if !defined(RK_SKIP_OPENCV) && defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
        const std::string paramPath =
            (std::filesystem::path(baseDir) / profile.detectorParam.relativePath).string();
        const std::string binPath =
            (std::filesystem::path(baseDir) / profile.detectorBin.relativePath).string();
        std::string cfgErr;
        const ScrfdNcnnConfig cfg =
            ScrfdNcnnAdapter::configFromProfile(profile, paramPath, binPath, cfgErr);
        if (!cfgErr.empty()) {
            // profile 装配非法（如 detectorOutputs 非 9 个）属输入契约问题
            err = "unsupported_input";
            return nullptr;
        }
        // 同一 profile 的另一条装配路径 decodeOptionsFromProfile 必须与 configFromProfile
        // 一致：两条路径若漂移，运行时解码语义会与检测配置不符（P1-5 防漂移检查）。
        const pipeline::ScrfdDecodeOptions decodeOpt =
            ScrfdNcnnAdapter::decodeOptionsFromProfile(profile);
        const bool decodeConsistent =
            static_cast<int>(decodeOpt.mode) == static_cast<int>(cfg.decodeMode) &&
            decodeOpt.numAnchors[0] == cfg.anchorsPerPos[0] &&
            decodeOpt.numAnchors[1] == cfg.anchorsPerPos[1] &&
            decodeOpt.numAnchors[2] == cfg.anchorsPerPos[2] &&
            decodeOpt.stride[0] == cfg.stride[0] && decodeOpt.stride[1] == cfg.stride[1] &&
            decodeOpt.stride[2] == cfg.stride[2];
        if (!decodeConsistent) {
            err = "unsupported_input";
            return nullptr;
        }
        auto det = std::make_shared<ScrfdNcnnAdapter>(cfg);
        std::string loadErr;
        if (!det->load(binPath, loadErr)) {
            err = reasonFromLoadError(loadErr);
            return nullptr;
        }
        return std::make_shared<ScrfdRealtimeAdapter>(std::move(det));
#else
        // 未编译 ncnn / SKIP_OPENCV 构建。⚠️ CI 的两个 job 恒命中此分支（见本函数头注释
        // 的「CI 覆盖缺口」）⇒ SCRFD 的真实装配接线在 CI 内**零覆盖**。
        err = "build_disabled";
        return nullptr;
#endif
    };

    // ArcFace 识别装配。P1-1：manifest 的 embedder 节点名（真实模型 in0/out0）必须注入
    // ArcFaceAdapter；其历史默认 data/output 用于旧调用方，直接沿用会导致真实模型
    // load/推理失败（自检通不过 → 管线无法发布）。
    h.loadEmbedder = [](const FaceRuntimeProfile& profile, const std::string& baseDir,
                        std::string& err) -> std::shared_ptr<IRealtimeEmbedder> {
#if !defined(RK_SKIP_OPENCV) && defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
        const std::string paramPath =
            (std::filesystem::path(baseDir) / profile.embedderParam.relativePath).string();
        // 缺口2：与检测侧 loadDetector 对称 —— 检测侧同时消费 detectorParam + detectorBin 并把
        // binPath 显式交给装配器；嵌入侧此前**只取 embedderParam**，bin 由 ArcFaceAdapter 按
        // param stem 自行推导，导致 profile.embedderBin 只被 verifyFiles 用于存在性/摘要校验、
        // **从未被装配消费**。当前无害仅因本仓 manifest 里两文件同 stem；一旦 bin 名与 param stem
        // 不同，就会静默加载到错误的 .bin。此处显式消费 manifest 声明的 embedderBin；声明缺失
        // （相对路径为空）时回落 ArcFaceAdapter 既有的 stem 推导（见 setModelBinPath 的空串语义）。
        const std::string binPath =
            profile.embedderBin.relativePath.empty()
                ? std::string()
                : (std::filesystem::path(baseDir) / profile.embedderBin.relativePath).string();
        auto emb = std::make_shared<ArcFaceAdapter>();
        // 注入 manifest 节点名（ncnn 节点名 / OpenCV blob 名同源对齐）；空串则保留默认。
        emb->setNodeNames(profile.embedderInput, profile.embedderOutput);
        // 缺口3：注入 manifest 声明的期望嵌入维度（setExpectedEmbeddingDim 对 <=0 忽略，保留默认）。
        emb->setExpectedEmbeddingDim(profile.embeddingDim);
        emb->setModelBinPath(binPath);  // 空串 ⇒ 适配器回落 stem 推导
        std::string loadErr;
        if (!emb->load(paramPath, loadErr)) {
            err = reasonFromLoadError(loadErr);
            return nullptr;
        }
        return std::make_shared<EmbedderRealtimeAdapter>(std::move(emb));
#else
        // 未编译 ncnn / SKIP_OPENCV 构建。⚠️ 与上面 SCRFD 同因：CI 的两个 job 恒命中此分支
        // （见本函数头注释的「CI 覆盖缺口」）⇒ ArcFace 的真实装配接线在 CI 内**零覆盖**。
        err = "build_disabled";
        return nullptr;
#endif
    };

    // 默认不加载图库（空图库合法）；宿主可注入真实加载器读取 FaceSearchLinearIndex。
    h.loadGallery = [](const FaceRuntimeProfile&, const std::string&,
                       std::string& err) -> std::shared_ptr<const FaceSearchLinearIndex> {
        err.clear();
        return nullptr;
    };

    // 自检：用合成图走一次检测 + 识别，校验无错误且嵌入维度符合 profile 契约。
    h.selfCheck = [](const RealtimePipelineSnapshot& snap, std::string& err) {
        if (!snap.detector || !snap.embedder) {
            err = "runtime_init_failed";
            return false;
        }
        const cv::Mat detImg =
            makeSyntheticBgr(snap.profile.detectorInputW, snap.profile.detectorInputH);
        std::vector<RealtimeFaceBox> boxes;
        std::string derr;
        if (!snap.detector->detect(detImg, boxes, derr)) {
            err = reasonFromLoadError(derr);
            return false;
        }
        const cv::Mat faceImg =
            makeSyntheticBgr(snap.profile.embedderInputW, snap.profile.embedderInputH);
        std::vector<float> emb;
        std::string eerr;
        if (!snap.embedder->embed(faceImg, emb, eerr)) {
            err = reasonFromLoadError(eerr);
            return false;
        }
        // 缺口3：即便已把 manifest 的 embeddingDim **注入** ArcFaceAdapter（见 loadEmbedder 的
        // setExpectedEmbeddingDim），此处仍按**实际** emb.size() 事后比对、不删：注入的是 manifest
        // **声明值**，模型实际输出维度仍可能与之不符（.param 与 manifest 漂移、bin/param 不匹配等），
        // 声明值无法自证。故用真实前向的输出维度做最后一道校验。
        if (snap.profile.embeddingDim > 0 &&
            static_cast<int>(emb.size()) != snap.profile.embeddingDim) {
            err = "unsupported_input";
            return false;
        }
        return true;
    };

    // 基准：高精度路径 10 预热 / 50 计时；每轮 = 检测 + 识别，取 p95。
    h.benchmark = [](const RealtimePipelineSnapshot& snap, const RealtimeBenchmarkConfig& cfg,
                     double& p95Ms, std::string& err) {
        if (!snap.detector || !snap.embedder) {
            err = "runtime_init_failed";
            return false;
        }
        const cv::Mat detImg =
            makeSyntheticBgr(snap.profile.detectorInputW, snap.profile.detectorInputH);
        const cv::Mat faceImg =
            makeSyntheticBgr(snap.profile.embedderInputW, snap.profile.embedderInputH);
        auto runOnce = [&](std::string& e) -> bool {
            std::vector<RealtimeFaceBox> boxes;
            if (!snap.detector->detect(detImg, boxes, e)) return false;
            std::vector<float> emb;
            return snap.embedder->embed(faceImg, emb, e);
        };
        const int warmup = std::max(0, cfg.warmupIters);
        const int timed = std::max(1, cfg.timedIters);
        for (int i = 0; i < warmup; ++i) {
            std::string e;
            if (!runOnce(e)) {
                err = reasonFromLoadError(e);
                return false;
            }
        }
        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(timed));
        for (int i = 0; i < timed; ++i) {
            const Clock::time_point t0 = Clock::now();
            std::string e;
            if (!runOnce(e)) {
                err = reasonFromLoadError(e);
                return false;
            }
            samples.push_back(durationMs(t0, Clock::now()));
        }
        p95Ms = percentile95(std::move(samples));
        return true;
    };

    return h;
}

RealtimeFacePipeline::RealtimeFacePipeline() : RealtimeFacePipeline(makeDefaultRealtimePipelineHooks()) {}

RealtimeFacePipeline::RealtimeFacePipeline(RealtimePipelineHooks hooks)
    : hooks_(std::move(hooks)) {
    // 头文件里 enforceBudget 恒为 false（平台中立、ODR 安全）；真实的平台默认值在此写入。
    bench_.enforceBudget = kDefaultEnforceBudget;
}

RealtimeFacePipeline::~RealtimeFacePipeline() {
    // 先等待所有 reloadAsync worker 结束再析构成员：worker 捕获 this，
    // 若在此前析构成员（hooks_/reloadMu_/snapshot_ 等）会发生 use-after-free。
    // worker 不回调本对象，故 join 不会自锁；此处阻塞仅发生在析构路径，
    // reloadAsync 本身仍是「立即返回 future」的非阻塞调用。
    std::vector<AsyncWorker> workers;
    {
        std::lock_guard<std::mutex> lock(asyncMu_);
        workers.swap(asyncWorkers_);
    }
    for (AsyncWorker& w : workers) {
        if (w.thread.joinable()) w.thread.join();
    }
}

void RealtimeFacePipeline::setBenchmarkConfig(const RealtimeBenchmarkConfig& cfg) {
    std::lock_guard<std::mutex> lock(cfgMu_);
    bench_ = cfg;
}

RealtimeBenchmarkConfig RealtimeFacePipeline::benchmarkConfig() const {
    std::lock_guard<std::mutex> lock(cfgMu_);
    return bench_;
}

std::shared_ptr<const RealtimePipelineSnapshot> RealtimeFacePipeline::snapshot() const {
    // 原子读：与 reload 的 atomic_store 配对；返回的 shared_ptr 使旧快照在本次使用期间存活。
    return std::atomic_load(&snapshot_);
}

std::uint64_t RealtimeFacePipeline::generation() const {
    return generation_.load(std::memory_order_acquire);
}

RealtimeReloadResult RealtimeFacePipeline::reload(const std::string& manifestJson,
                                                  const std::string& baseDir,
                                                  const std::string& currentAbi,
                                                  const std::string& galleryDir) {
    std::lock_guard<std::mutex> lock(reloadMu_);

    // 复制钩子/基准配置，避免与 setBenchmarkConfig 竞争。
    RealtimePipelineHooks hooks;
    RealtimeBenchmarkConfig bench;
    {
        std::lock_guard<std::mutex> cfgLock(cfgMu_);
        hooks = hooks_;
        bench = bench_;
    }

    FaceRuntimeProfile profile;
    std::string err;
    if (!FaceRuntimeManifest::parseV1(manifestJson, currentAbi, profile, err)) {
        return failResult("unsupported_input", "manifest 解析失败: " + err, generation());
    }
    if (hooks.verifyFiles && !hooks.verifyFiles(profile, baseDir, err)) {
        // reason 按 err 语义分类（见 reasonFromVerifyError），不再一律硬编码 missing_model：
        // verifyFiles 的四类失败语义不同（文件缺失 vs 路径非法 / LFS 指针 / 摘要不符），
        // 全报 missing_model 会让排障者朝「去补模型文件」这一错误方向查。
        // 注：err 文本本身一直随 evidence 透传到调用方（未丢弃），这里修的只是 reason 码。
        const std::string reason = reasonFromVerifyError(err);
        return failResult(reason, "模型文件校验失败: " + err, generation());
    }
    return buildAndPublishLocked(profile, baseDir, currentAbi, galleryDir, hooks, bench);
}

RealtimeReloadResult RealtimeFacePipeline::buildAndPublishLocked(
    const FaceRuntimeProfile& profile, const std::string& baseDir, const std::string& abi,
    const std::string& galleryDir, RealtimePipelineHooks& hooks, RealtimeBenchmarkConfig bench) {
    // ── 1) 装配候选 detector / embedder（解析加载阶段，可与推理并发） ──
    std::string err;
    std::shared_ptr<IRealtimeDetector> detector =
        hooks.loadDetector ? hooks.loadDetector(profile, baseDir, err) : nullptr;
    if (!detector) {
        const std::string reason = reasonFromLoadError(err);
        return failResult(reason, "detector 装配失败: " + err, generation());
    }
    err.clear();
    std::shared_ptr<IRealtimeEmbedder> embedder =
        hooks.loadEmbedder ? hooks.loadEmbedder(profile, baseDir, err) : nullptr;
    if (!embedder) {
        const std::string reason = reasonFromLoadError(err);
        return failResult(reason, "embedder 装配失败: " + err, generation());
    }
    err.clear();
    std::shared_ptr<const FaceSearchLinearIndex> gallery;
    if (hooks.loadGallery) {
        gallery = hooks.loadGallery(profile, galleryDir, err);
        // 图库是识别增强，非必需：nullptr + 空 err 视为合法空图库。
        if (!gallery && !err.empty()) {
            // 与上面 detector/embedder 分支同口径：reason 由 err 归一得出，不再硬编码
            // missing_model（FACE-PIPE-010 ②）。
            // ⚠️ 现状**生产不可达**（如实记录，勿高估其价值）：本仓 loadGallery 的赋值点
            //    都不写 err —— 默认钩子见本文件 makeDefaultRealtimePipelineHooks（err.clear()
            //    后返回 nullptr），Android 生产钩子见 native_face_bridge.cpp 的
            //    makeActivatedHooks（err.clear() 后返回内存图库快照）。
            //    故本分支目前只在「自定义 loadGallery 主动写 err」时才触发，属**防御性一致性**
            //    改动，不是活跃缺陷；将来若有钩子开始写 err，reason 才不会退化成硬编码值。
            const std::string reason = reasonFromLoadError(err);
            return failResult(reason, "图库加载失败: " + err, generation());
        }
    }

    // ── 2) 候选快照（尚未发布，effective=false，失败即丢弃不影响旧快照） ──
    auto candidate = std::make_shared<RealtimePipelineSnapshot>();
    candidate->profile = profile;
    candidate->detector = std::move(detector);
    candidate->embedder = std::move(embedder);
    candidate->gallery = std::move(gallery);
    candidate->requested = true;
    candidate->effective = false;
    candidate->reason = "runtime_init_failed";
    candidate->evidence = "profile=" + profile.profileId + " abi=" + abi;

    // ── 3) 自检推理 ──
    std::string checkErr;
    if (!hooks.selfCheck || !hooks.selfCheck(*candidate, checkErr)) {
        const std::string reason =
            checkErr.empty() ? "runtime_init_failed" : reasonFromLoadError(checkErr);
        return failResult(reason, "自检推理失败: " + checkErr, generation());
    }

    // ── 4) 基准：高精度 10 预热 / 50 计时；ARM64 p95 ≤ 250ms ──
    double p95 = 0.0;
    std::string benchErr;
    if (!hooks.benchmark || !hooks.benchmark(*candidate, bench, p95, benchErr)) {
        const std::string reason =
            benchErr.empty() ? "runtime_init_failed" : reasonFromLoadError(benchErr);
        return failResult(reason, "基准失败: " + benchErr, generation());
    }
    if (bench.enforceBudget && p95 > bench.p95BudgetMs) {
        return failResult("runtime_init_failed",
                          "基准超预算: p95_ms=" + fmtMs(p95) + " > budget_ms=" +
                              fmtMs(bench.p95BudgetMs),
                          generation());
    }

    // ── 5) 发布：先 atomic_store 新快照，再自增 generation_ ──
    // 顺序不可颠倒：若先自增 generation_ 再发布，读侧会在新快照可见前短暂读到
    // generation()=N+1 而 snapshot()->generation=N（代次领先于快照，自相矛盾）。
    // 先发布后自增，则读侧至多看到「快照已前进而 generation() 暂滞后一步」，
    // 绝不会读到不存在的代次。reloadMu_ 串行化所有发布，load+1 无写入竞争。
    //
    // ⚠️ 本顺序**现有判别性用例**（tests/cpp/test_realtime_face_pipeline.cpp 的
    //    PublishOrderInvariantSnapshotNeverBehindGeneration）：用例经下面注入的 publishGapHook
    //    在上述两行**之间**采样不变量 `snapshot()->generation >= generation()`。
    //    · 正确顺序：钩子点处 snapshot()->generation==gen（新快照已发布）而 generation()==gen-1
    //      ⇒ gen >= gen-1 成立；
    //    · 颠倒两行：钩子点处 generation()==gen 而 snapshot()->generation==gen-1（仍指旧快照）
    //      ⇒ 不变量破 ⇒ 用例确定性变红。用例另以 sawGap 标志防「钩子未被调用 ⇒ 空转假绿」。
    candidate->effective = true;
    candidate->reason = "ok";
    candidate->benchmarkP95Ms = p95;
    candidate->evidence += " p95_ms=" + fmtMs(p95) +
                           " budget_ms=" + fmtMs(bench.p95BudgetMs) +
                           " warmup=" + std::to_string(bench.warmupIters) +
                           " iters=" + std::to_string(bench.timedIters);
    const std::uint64_t gen = generation_.load(std::memory_order_acquire) + 1;
    candidate->generation = gen;
    std::atomic_store(&snapshot_, std::shared_ptr<const RealtimePipelineSnapshot>(candidate));
    // 发布间隙测试接缝：生产恒为空；非空时在两行之间采样（见 .h 中 publishGapHook 说明）。
    if (hooks.publishGapHook) hooks.publishGapHook();
    generation_.store(gen, std::memory_order_release);

    RealtimeReloadResult r;
    r.ok = true;
    r.reason = "ok";
    r.evidence = candidate->evidence;
    r.generation = gen;
    r.benchmarkP95Ms = p95;
    return r;
}

std::future<RealtimeReloadResult> RealtimeFacePipeline::reloadAsync(
    std::string manifestJson, std::string baseDir, std::string currentAbi,
    std::string galleryDir) {
    // 工作线程执行阻塞式 reload；句柄登记于 asyncWorkers_，析构时 join 以消除
    // worker 捕获 this 的 UAF。本函数只登记、不等待，立即返回 future（非阻塞）。
    auto promise = std::make_shared<std::promise<RealtimeReloadResult>>();
    std::future<RealtimeReloadResult> future = promise->get_future();
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([this, promise, done, manifestJson = std::move(manifestJson),
                 baseDir = std::move(baseDir), currentAbi = std::move(currentAbi),
                 galleryDir = std::move(galleryDir)]() mutable {
        try {
            promise->set_value(this->reload(manifestJson, baseDir, currentAbi, galleryDir));
        } catch (const std::exception& e) {
            promise->set_value(failResult("runtime_init_failed", e.what(),
                                          this->generation()));
        } catch (...) {
            promise->set_value(failResult("runtime_init_failed", "unknown error",
                                          this->generation()));
        }
        // 值已写入 promise 后才标记结束，保证 join 返回时 future 必已就绪。
        done->store(true, std::memory_order_release);
    });
    {
        std::lock_guard<std::mutex> lock(asyncMu_);
        // 顺带回收已结束的 worker，避免句柄表随调用次数无限增长。
        for (auto it = asyncWorkers_.begin(); it != asyncWorkers_.end();) {
            if (it->done->load(std::memory_order_acquire)) {
                if (it->thread.joinable()) it->thread.join();
                it = asyncWorkers_.erase(it);
            } else {
                ++it;
            }
        }
        asyncWorkers_.push_back(AsyncWorker{std::move(worker), std::move(done)});
    }
    return future;
}

bool RealtimeFacePipeline::detect(const cv::Mat& bgr, std::vector<RealtimeFaceBox>& out,
                                  std::string& err) const {
    // 原子取快照并持有整个调用：即使并发 reload 替换，旧 detector 也不会在使用中被销毁。
    const std::shared_ptr<const RealtimePipelineSnapshot> snap = snapshot();
    if (!snap || !snap->detector) {
        err = "runtime_init_failed";
        return false;
    }
    return snap->detector->detect(bgr, out, err);
}

bool RealtimeFacePipeline::embed(const cv::Mat& alignedFaceBgr, std::vector<float>& out,
                                 std::string& err) const {
    const std::shared_ptr<const RealtimePipelineSnapshot> snap = snapshot();
    if (!snap || !snap->embedder) {
        err = "runtime_init_failed";
        return false;
    }
    return snap->embedder->embed(alignedFaceBgr, out, err);
}

}  // namespace rk_core
