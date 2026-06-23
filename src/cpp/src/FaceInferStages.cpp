#include "FaceInferStages.h"

#include "ArcFaceEmbedder.h"
#include "FaceAlign.h"
#include "FaceTemplate.h"
#include "ModelRegistry.h"
#include "NativeLog.h"
#include "YoloFaceDetector.h"

#include <opencv2/imgcodecs.hpp>

#include <chrono>
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

std::vector<float> FaceInferStages::makeFakeEmbedding512ForTest(const cv::Rect2f& bbox, const cv::Size& imgSz) {
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

    std::string detErr;
    std::unique_ptr<YoloFaceDetector> det;

    // INT8 量化模型优先选择
    if (req.int8Enabled) {
        ModelRegistry::ensureBuiltinRegistered();
        auto int8Det = ModelRegistry::instance().createDetector("yolo_face_int8");
        if (int8Det) {
            std::string loadErr;
            if (int8Det->load(req.yoloModelPath, loadErr)) {
                ctx.yoloBackendName = std::string(int8Det->name()) + " (INT8)";
                const auto td0 = std::chrono::steady_clock::now();
                ctx.faces = int8Det->detect(ctx.img, detErr);
                const auto td1 = std::chrono::steady_clock::now();
                m.msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count();
                if (detErr.empty()) return okStatus();
            }
            // INT8 加载失败，回退到 FP32
        }
    }

    if (req.yoloBackend == "opencv" || req.yoloBackend == "opencv_dnn" || req.yoloBackend == "ncnn") {
        ModelRegistry::ensureBuiltinRegistered();
        auto adapter = ModelRegistry::instance().createDetector(req.yoloBackend);
        if (!adapter) {
            return failStatus("yolo_load", "detector_not_found: " + req.yoloBackend);
        }
        std::string loadErr;
        if (!adapter->load(req.yoloModelPath, loadErr)) {
            return failStatus("yolo_load", loadErr.empty() ? "yolo_load_failed" : loadErr);
        }
        ctx.yoloBackendName = adapter->name();
        const auto td0 = std::chrono::steady_clock::now();
        ctx.faces = adapter->detect(ctx.img, detErr);
        const auto td1 = std::chrono::steady_clock::now();
        m.msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count();
        if (!detErr.empty()) {
            return failStatus("yolo_detect", detErr);
        }
        return okStatus();
    } else if (req.yoloBackend == "qualcomm") {
        rklog::logInfo("FaceInferStages", "yoloBackend", "Qualcomm SDK fallback to CPU... 待补测");
        det = CreateOpenCvDnnYoloFaceDetector();
        YoloFaceModelSpec spec;
        spec.modelPath = req.yoloModelPath;
        spec.configPath = req.yoloConfigPath;
        spec.framework = req.yoloFramework;
        spec.outputName = req.yoloOutputName;

        YoloFaceOptions opt;
        opt.inputW = req.yoloInputW;
        opt.inputH = req.yoloInputH;
        opt.scoreThreshold = req.yoloScoreThreshold;
        opt.nmsIouThreshold = req.yoloNmsIouThreshold;
        opt.enableKeypoints5 = req.yoloEnableKeypoints5;
        opt.letterbox = req.yoloLetterbox;
        opt.swapRB = req.yoloSwapRB;
        opt.scale = req.yoloScale;
        opt.meanB = req.yoloMeanB;
        opt.meanG = req.yoloMeanG;
        opt.meanR = req.yoloMeanR;
        opt.opencvBackend = req.yoloOpenCvBackend;
        opt.opencvTarget = req.yoloOpenCvTarget;

        std::string loadErr;
        if (!det->load(spec, opt, loadErr)) {
            return failStatus("yolo_load", loadErr.empty() ? "yolo_load_failed" : loadErr);
        }
    } else {
        return failStatus("yolo_load", "yolo_backend_unsupported");
    }

    ctx.yoloBackendName = det ? det->backendName() : "";
    const auto td0 = std::chrono::steady_clock::now();
    ctx.faces = det ? det->detect(ctx.img, detErr) : FaceDetections{};
    const auto td1 = std::chrono::steady_clock::now();
    m.msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count();
    if (!detErr.empty()) {
        return failStatus("yolo_detect", detErr);
    }
    return okStatus();
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
        ctx.embedding = makeFakeEmbedding512ForTest(ctx.mainFace.bbox, ctx.img.size());
        ctx.embeddingOk = (ctx.embedding.size() == static_cast<size_t>(ArcFaceEmbedding::kDim));
        ctx.embeddingFake = true;
        const auto te1 = std::chrono::steady_clock::now();
        m.msEmbed = std::chrono::duration_cast<std::chrono::milliseconds>(te1 - te0).count();
        return okStatus();
    }

    // INT8 量化模型优先选择
    if (req.int8Enabled) {
        ModelRegistry::ensureBuiltinRegistered();
        // 按优先级尝试 INT8 识别器
        const char* int8EmbedderIds[] = {"arcface_int8", "mobilefacenet_int8"};
        for (const char* eId : int8EmbedderIds) {
            auto int8Emb = ModelRegistry::instance().createEmbedder(eId);
            if (int8Emb) {
                std::string loadErr;
                if (int8Emb->load(req.arcModelPath, loadErr)) {
                    std::string embedErr;
                    auto e = int8Emb->embed(ctx.aligned112, embedErr);
                    if (e.has_value()) {
                        ctx.embedding = std::move(*e);
                        ctx.embeddingOk = (ctx.embedding.size() == static_cast<size_t>(ArcFaceEmbedding::kDim));
                        ctx.embeddingFake = false;
                        ctx.arcBackendName = std::string(int8Emb->name()) + " (INT8)";
                        const auto te1 = std::chrono::steady_clock::now();
                        m.msEmbed = std::chrono::duration_cast<std::chrono::milliseconds>(te1 - te0).count();
                        return okStatus();
                    }
                }
                // INT8 加载/推理失败，尝试下一个
            }
        }
    }

    ArcFaceEmbedderConfig cfg;
    cfg.inputW = req.arcInputW;
    cfg.inputH = req.arcInputH;
    cfg.modelVersion = req.arcModelVersion;
    cfg.preprocessVersion = req.arcPreprocessVersion;
    cfg.opencvModel = req.arcModelPath;
    cfg.opencvConfig = req.arcConfigPath;
    cfg.opencvFramework = req.arcFramework;
    cfg.opencvOutput = req.arcOutputName;
    cfg.opencvInput = req.arcInputName;

    if (req.arcBackend == "opencv" || req.arcBackend == "opencv_dnn") {
        cfg.backend = ArcFaceEmbedderConfig::BackendType::OpenCvDnn;
    } else if (req.arcBackend == "ncnn") {
        cfg.backend = ArcFaceEmbedderConfig::BackendType::Ncnn;
        cfg.ncnnParam = req.arcNcnnParam;
        cfg.ncnnBin = req.arcNcnnBin;
        cfg.ncnnInput = req.arcNcnnInput;
        cfg.ncnnOutput = req.arcNcnnOutput;
        cfg.ncnnThreads = req.arcNcnnThreads;
        cfg.ncnnLightmode = req.arcNcnnLightmode;
    } else if (req.arcBackend == "qualcomm") {
        cfg.backend = ArcFaceEmbedderConfig::BackendType::Qualcomm;
        cfg.qualcommModel = req.arcModelPath;
    } else {
        return failStatus("arc_init", "arc_backend_unsupported");
    }

    ArcFaceEmbedder emb;
    std::string initErr;
    if (!emb.initialize(cfg, &initErr)) {
        return failStatus("arc_init", initErr.empty() ? "arc_init_failed" : initErr);
    }

    if (cfg.backend == ArcFaceEmbedderConfig::BackendType::OpenCvDnn) {
        ctx.arcBackendName = "opencv_dnn";
    } else if (cfg.backend == ArcFaceEmbedderConfig::BackendType::Ncnn) {
        ctx.arcBackendName = "ncnn";
    } else if (cfg.backend == ArcFaceEmbedderConfig::BackendType::Qualcomm) {
        ctx.arcBackendName = "qualcomm";
    } else {
        ctx.arcBackendName = "unknown";
    }

    std::string embedErr;
    auto e = emb.embedAlignedFaceBgr(ctx.aligned112, &embedErr);
    if (!e.has_value()) {
        return failStatus("arc_embed", embedErr.empty() ? "arc_embed_failed" : embedErr);
    }
    ctx.embedding = std::move(e->values);
    ctx.embeddingOk = (ctx.embedding.size() == static_cast<size_t>(ArcFaceEmbedding::kDim));
    ctx.embeddingFake = false;

    const auto te1 = std::chrono::steady_clock::now();
    m.msEmbed = std::chrono::duration_cast<std::chrono::milliseconds>(te1 - te0).count();
    return okStatus();
}

FaceInferStageStatus FaceInferStages::loadGallery(const FaceInferRequest& req, FaceInferContext& ctx) {
    std::string galleryErr;
    if (!loadGalleryDir(req.galleryDir, ctx.galleryEntries, ctx.galleryWarnings, galleryErr)) {
        ctx.galleryEntries.clear();
        return failStatus("gallery_load", galleryErr.empty() ? "gallery_load_failed" : galleryErr);
    }
    return okStatus();
}

FaceInferStageStatus FaceInferStages::searchTopK(const FaceInferRequest& req, FaceInferContext& ctx, FaceInferMetrics& m) {
    if (!(ctx.hasFace && ctx.embeddingOk)) return okStatus();

    std::string searchErr;
    const auto ts0 = std::chrono::steady_clock::now();
    if (!ctx.index.reset(std::move(ctx.galleryEntries), static_cast<size_t>(ArcFaceEmbedding::kDim), searchErr)) {
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

