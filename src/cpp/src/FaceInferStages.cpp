#include "FaceInferStages.h"

using namespace rk_core;

#include "ArcFaceEmbedder.h"
#include "FaceAlign.h"
#include "FaceTemplate.h"
#include "ModelRegistry.h"
#include "NativeLog.h"
#include "YoloFaceDetector.h"
// INT8 链的 `arcface_int8` 分支需要对适配器做具体类型注入（详见 computeEmbedding 内注释）。
#include "adapters/ArcFaceAdapter.h"

#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

static FaceInferStageStatus okStatus() {
    return {};
}

static FaceInferStageStatus failStatus(const char* stage, const std::string& msg) {
    FaceInferStageStatus s;
    s.ok = false;
    s.stage = stage ? stage : "";
    s.message = msg;
    return s;
}

static std::string lowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

static bool l2NormalizeInplace(std::vector<float>& v) {
    if (v.empty()) return false;
    float s = 0.0f;
    for (float x : v) s += x * x;
    if (!(s > 0.0f) || !std::isfinite(s)) return false;
    const float n = std::sqrt(s);
    if (!(n > 0.0f) || !std::isfinite(n)) return false;
    const float inv = 1.0f / n;
    for (float& x : v) x *= inv;
    return true;
}

static FaceDetections fakeDetectCenterFace(const cv::Mat& bgr) {
    if (bgr.empty()) return {};
    const float w = static_cast<float>(bgr.cols);
    const float h = static_cast<float>(bgr.rows);
    const float bw = std::max(1.0f, w * 0.45f);
    const float bh = std::max(1.0f, h * 0.45f);
    const float x = (w - bw) * 0.5f;
    const float y = (h - bh) * 0.5f;

    FaceDetection d;
    d.bbox = cv::Rect2f(x, y, bw, bh);
    d.score = 1.0f;
    return {d};
}

static bool readAllBytes(const std::filesystem::path& p, std::vector<std::uint8_t>& out, std::string& err) {
    err.clear();
    out.clear();
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) {
        err = "open_failed";
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0) {
        err = "empty_file";
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(out.data()), n);
    if (!f) {
        err = "read_failed";
        return false;
    }
    return true;
}

static bool loadGalleryDir(const std::string& dir,
                           std::vector<FaceSearchEntry>& entriesOut,
                           std::vector<std::string>& warningsOut,
                           std::string& err) {
    err.clear();
    entriesOut.clear();
    warningsOut.clear();
    if (dir.empty()) return true;

    std::filesystem::path root(dir);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return true;
    if (!std::filesystem::is_directory(root, ec)) {
        err = "gallery_not_directory";
        return false;
    }

    for (const auto& it : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!it.is_regular_file()) continue;

        const auto p = it.path();
        const std::string id = p.stem().string();
        if (id.empty()) continue;

        std::vector<std::uint8_t> bytes;
        std::string ioErr;
        if (!readAllBytes(p, bytes, ioErr)) {
            warningsOut.push_back(id + ":io:" + ioErr);
            continue;
        }

        std::string parseErr;
        auto t = deserializeFaceTemplate(bytes, &parseErr);
        if (!t.has_value()) {
            warningsOut.push_back(id + ":parse:" + parseErr);
            continue;
        }

        FaceSearchEntry e;
        e.id = id;
        e.embedding = std::move(t->embedding);
        if (e.embedding.size() != static_cast<size_t>(ArcFaceEmbedding::kDim)) {
            warningsOut.push_back(id + ":dim_mismatch");
            continue;
        }
        entriesOut.push_back(std::move(e));
    }

    if (ec) {
        err = "gallery_iter_failed";
        return false;
    }
    return true;
}

static size_t pickMainFaceIndex(const FaceInferRequest& req, const FaceDetections& faces) {
    if (faces.empty()) return 0;
    const std::string p = lowerAscii(req.faceSelectPolicy);

    if (p == "first") return 0;
    size_t bestIdx = 0;
    if (p == "area" || p == "largest") {
        for (size_t i = 1; i < faces.size(); i++) {
            const auto& a = faces[i];
            const auto& b = faces[bestIdx];
            const float areaA = a.bbox.width * a.bbox.height;
            const float areaB = b.bbox.width * b.bbox.height;
            if (areaA > areaB) bestIdx = i;
        }
        return bestIdx;
    }

    for (size_t i = 1; i < faces.size(); i++) {
        const auto& a = faces[i];
        const auto& b = faces[bestIdx];
        const float areaA = a.bbox.width * a.bbox.height;
        const float areaB = b.bbox.width * b.bbox.height;
        if (a.score > b.score) bestIdx = i;
        else if (a.score == b.score && areaA > areaB) bestIdx = i;
    }
    return bestIdx;
}

}  // namespace

std::vector<float> FaceInferStages::makeFakeEmbedding512(const cv::Rect2f& bbox, const cv::Size& imgSz) {
    std::vector<float> v(static_cast<size_t>(ArcFaceEmbedding::kDim), 0.0f);
    const float sx = imgSz.width > 0 ? (bbox.x / static_cast<float>(imgSz.width)) : 0.0f;
    const float sy = imgSz.height > 0 ? (bbox.y / static_cast<float>(imgSz.height)) : 0.0f;
    const float sw = imgSz.width > 0 ? (bbox.width / static_cast<float>(imgSz.width)) : 0.0f;
    const float sh = imgSz.height > 0 ? (bbox.height / static_cast<float>(imgSz.height)) : 0.0f;
    const float seed = 0.31f * sx + 0.17f * sy + 0.13f * sw + 0.11f * sh;
    for (size_t i = 0; i < v.size(); i++) {
        const float x = static_cast<float>(i) * 0.0137f + seed * 3.7f;
        v[i] = std::sin(x) * 0.7f + std::cos(x * 1.9f) * 0.3f;
    }
    if (!l2NormalizeInplace(v)) {
        std::fill(v.begin(), v.end(), 0.0f);
    }
    return v;
}

FaceInferStageStatus FaceInferStages::loadImage(const FaceInferRequest& req, FaceInferContext& ctx, FaceInferMetrics& m) {
    const auto t0 = std::chrono::steady_clock::now();
    ctx.img = cv::imread(req.imagePath, cv::IMREAD_COLOR);
    const auto t1 = std::chrono::steady_clock::now();
    m.msLoad = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    if (ctx.img.empty()) {
        return failStatus("image_load", "image_load_failed");
    }
    return okStatus();
}

FaceInferStageStatus FaceInferStages::detectFaces(const FaceInferRequest& req, FaceInferContext& ctx, FaceInferMetrics& m) {
    const std::string p = lowerAscii(req.faceSelectPolicy);
    if (req.fakeDetect) {
        ctx.yoloBackendName = "fake";
        ctx.faces = (p == "fake_none") ? FaceDetections{} : fakeDetectCenterFace(ctx.img);
        m.msDetect = 0;
        return okStatus();
    }

    // 使用外部注入的检测器实例（调用方管理生命周期，每帧复用避免重复加载模型）
    if (ctx.detector) {
        ctx.yoloBackendName = ctx.detector->name();
        std::string detErr;
        const auto td0 = std::chrono::steady_clock::now();
        ctx.faces = ctx.detector->detect(ctx.img, detErr);
        const auto td1 = std::chrono::steady_clock::now();
        m.msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count();
        if (!detErr.empty()) {
            return failStatus("yolo_detect", detErr);
        }
        return okStatus();
    }

    // §I3：装配集中在 runFaceInferOnce（FaceInferencePipeline.cpp）——本阶段**只消费注入实例**。
    // 未注入（含装配失败）时带出真实装配错误，**不再**自行 createDetector / 回落：
    // 原「同路径 INT8 候选队列」与「按 yoloBackend 查注册表的第二套逻辑」已按本轮显式决策删除
    // （替代关系由 §I3/A16 决策确立：一个明确模型 ID 绑定当前单一路径，加载失败明确报错）。
    return failStatus("yolo_load",
                      ctx.detectorLoadError.empty() ? "detector_unavailable" : ctx.detectorLoadError);
}

void FaceInferStages::selectMainFace(const FaceInferRequest& req, FaceInferContext& ctx) {
    ctx.hasFace = !ctx.faces.empty();
    if (!ctx.hasFace) return;
    const size_t bestIdx = pickMainFaceIndex(req, ctx.faces);
    ctx.mainFace = ctx.faces[bestIdx];
}

FaceInferStageStatus FaceInferStages::alignFace(const FaceInferRequest& req, FaceInferContext& ctx, FaceInferMetrics& m) {
    if (!ctx.hasFace) return okStatus();

    const auto ta0 = std::chrono::steady_clock::now();
    FaceAlignOptions aopt;
    aopt.outW = req.arcInputW;
    aopt.outH = req.arcInputH;
    aopt.preferKeypoints5 = true;
    auto ar = alignFaceForArcFace112(ctx.img, ctx.mainFace, aopt);
    const auto ta1 = std::chrono::steady_clock::now();
    m.msAlign = std::chrono::duration_cast<std::chrono::milliseconds>(ta1 - ta0).count();

    if (ar.alignedBgr.empty()) {
        return failStatus("align", ar.err.empty() ? "align_failed" : ar.err);
    }

    ctx.aligned112 = std::move(ar.alignedBgr);
    ctx.usedKeypoints5 = ar.usedKeypoints5;
    ctx.usedBboxFallback = ar.usedBboxFallback;
    return okStatus();
}

FaceInferStageStatus FaceInferStages::computeEmbedding(const FaceInferRequest& req, FaceInferContext& ctx, FaceInferMetrics& m) {
    if (!ctx.hasFace) return okStatus();

    const auto te0 = std::chrono::steady_clock::now();
    if (req.fakeEmbedding) {
        ctx.embedding = makeFakeEmbedding512(ctx.mainFace.bbox, ctx.img.size());
        ctx.embeddingOk = (ctx.embedding.size() == static_cast<size_t>(ArcFaceEmbedding::kDim));
        ctx.embeddingFake = true;
        const auto te1 = std::chrono::steady_clock::now();
        m.msEmbed = std::chrono::duration_cast<std::chrono::milliseconds>(te1 - te0).count();
        // ⚠️ 同形状登记（2026-09-26 验收 A2 指出，**非本轮引入、本轮不改**）：本行先算 `embeddingOk`
        //    再**无条件**返回 ok，属**潜伏**的「失败静默」形态（当前不可触发：`makeFakeEmbedding512`
        //    恒返回 `kDim`）；将来若该函数返回维度变化，须按下方注入分支的做法把原因写进 err 并走失败
        //    路径，保持一致口径。
        return okStatus();
    }

    // §I3：装配集中在 runFaceInferOnce（FaceInferencePipeline.cpp），本阶段**只消费注入的
    // Embedder 实例**。统一接口 `Embedder::embed()` **不保证** 512 维（旧 `ArcFaceEmbedder*`
    // 的「has_value() 蕴含 512 维」静态保证对派生实现不再成立）⇒ 必须**显式校验维度**：
    // 不符即失败（不静默）、返回不含身份，避免下游把错维向量当成功继续检索。
    if (ctx.embedder) {
        std::string embedErr;
        auto e = ctx.embedder->embed(ctx.aligned112, embedErr);
        if (e.has_value()) {
            if (e->size() == static_cast<size_t>(ArcFaceEmbedding::kDim)) {
                ctx.embedding = std::move(*e);
                ctx.embeddingOk = true;
                ctx.arcBackendName = ctx.embedder->name();
                const auto te1 = std::chrono::steady_clock::now();
                m.msEmbed = std::chrono::duration_cast<std::chrono::milliseconds>(te1 - te0).count();
                return okStatus();
            }
            return failStatus("arc_embed",
                              "embedding_dim_mismatch（实际 " + std::to_string(e->size()) + "）");
        }
        return failStatus("arc_embed", embedErr.empty() ? "arc_embed_failed" : embedErr);
    }

    // §I3：未注入（含装配失败）时带出真实装配错误，**不再**自行 createEmbedder / 回落 FP32/INT8。
    // 原「同路径 INT8 候选队列（arcface_int8 → mobilefacenet_int8）+ FP32 回落链」已按本轮显式
    // 决策删除（替代关系见 §I3/A16：一个明确模型 ID 绑定当前单一路径，加载失败明确报错；
    // 绝不把同一文件依次交给 ArcFace、MobileFaceNet）。
    return failStatus("arc_init",
                      ctx.embedderLoadError.empty() ? "embedder_unavailable" : ctx.embedderLoadError);
}

FaceInferStageStatus FaceInferStages::loadGallery(const FaceInferRequest& req, FaceInferContext& ctx) {
    // §I3：内存快照优先——req.gallerySnapshot 非空时直接绑定该不可变索引，**不读磁盘**
    //（Android 侧已解密并经 replaceGallery 显式绑定空间的内存图库）。旧明文目录夹具仅保留
    // 研究兼容；不得把 Android 加密目录交给 loadGalleryDir。
    if (req.gallerySnapshot) {
        if (req.gallerySnapshotSpaceId != req.templateSpaceId) {
            return failStatus("gallery_load", "gallery_space_mismatch");
        }
        ctx.gallerySnapshot = req.gallerySnapshot;
        return okStatus();
    }
    std::string galleryErr;
    std::filesystem::path dir(req.galleryDir);
    if (!req.templateSpaceId.empty() && !req.galleryDir.empty()) {
        if (req.templateSpaceId.size() != 64 || req.templateSpaceId.find_first_not_of("0123456789abcdef") != std::string::npos) {
            return failStatus("gallery_load", "gallery_space_invalid");
        }
        dir /= "spaces";
        dir /= req.templateSpaceId;
    }
    if (!loadGalleryDir(dir.string(), ctx.galleryEntries, ctx.galleryWarnings, galleryErr)) {
        ctx.galleryEntries.clear();
        return failStatus("gallery_load", galleryErr.empty() ? "gallery_load_failed" : galleryErr);
    }
    return okStatus();
}

FaceInferStageStatus FaceInferStages::searchTopK(const FaceInferRequest& req, FaceInferContext& ctx, FaceInferMetrics& m) {
    if (!(ctx.hasFace && ctx.embeddingOk)) return okStatus();

    std::string searchErr;
    const auto ts0 = std::chrono::steady_clock::now();

    // §I3：内存快照直接检索其**不可变**索引，不重复复制模板。
    if (ctx.gallerySnapshot) {
        if (ctx.gallerySnapshot->size() > 0) {
            FaceSearchOptions opt;
            opt.assumeL2Normalized = req.assumeL2Normalized;
            ctx.hits = ctx.gallerySnapshot->searchTopK(ctx.embedding, req.topK, opt, searchErr);
            if (!searchErr.empty()) {
                const auto ts1 = std::chrono::steady_clock::now();
                m.msSearch = std::chrono::duration_cast<std::chrono::milliseconds>(ts1 - ts0).count();
                return failStatus("search", searchErr);
            }
        }
        const auto ts1 = std::chrono::steady_clock::now();
        m.msSearch = std::chrono::duration_cast<std::chrono::milliseconds>(ts1 - ts0).count();
        if (!ctx.hits.empty()) {
            ctx.bestScore = ctx.hits[0].score;
            ctx.hasCandidate = true;
        } else {
            ctx.bestScore = -1.0f;
            ctx.hasCandidate = false;
        }
        return okStatus();
    }

    // Copy entries so a failed reset() doesn't leave galleryEntries empty for a retry
    auto entries = ctx.galleryEntries;
    if (!ctx.index.reset(std::move(entries), static_cast<size_t>(ArcFaceEmbedding::kDim), searchErr)) {
        const auto ts1 = std::chrono::steady_clock::now();
        m.msSearch = std::chrono::duration_cast<std::chrono::milliseconds>(ts1 - ts0).count();
        ctx.hits.clear();
        ctx.hasCandidate = false;
        ctx.bestScore = -1.0f;
        return failStatus("search", searchErr.empty() ? "index_reset_failed" : searchErr);
    }

    if (ctx.index.size() > 0) {
        FaceSearchOptions opt;
        opt.assumeL2Normalized = req.assumeL2Normalized;
        ctx.hits = ctx.index.searchTopK(ctx.embedding, req.topK, opt, searchErr);
        if (!searchErr.empty()) {
            const auto ts1 = std::chrono::steady_clock::now();
            m.msSearch = std::chrono::duration_cast<std::chrono::milliseconds>(ts1 - ts0).count();
            return failStatus("search", searchErr);
        }
    }

    const auto ts1 = std::chrono::steady_clock::now();
    m.msSearch = std::chrono::duration_cast<std::chrono::milliseconds>(ts1 - ts0).count();
    if (!ctx.hits.empty()) {
        ctx.bestScore = ctx.hits[0].score;
        ctx.hasCandidate = true;
    } else {
        ctx.bestScore = -1.0f;
        ctx.hasCandidate = false;
    }
    return okStatus();
}

ThresholdDecisionResult FaceInferStages::makeDecision(const FaceInferRequest& req, const FaceInferContext& ctx) {
    ThresholdPolicyVersion v;
    v.versionId = req.thresholdVersionId;
    v.acceptThreshold = req.acceptThreshold;
    v.consecutivePassesToTrigger = req.consecutivePassesToTrigger > 0 ? static_cast<size_t>(req.consecutivePassesToTrigger) : 1;
    ThresholdDecisionPolicy policy(v);
    return policy.feed(ctx.bestScore, ctx.hasCandidate);
}

