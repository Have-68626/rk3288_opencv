#if __has_include("FaceInferencePipeline.h")
#include "FaceInferencePipeline.h"
#else
#include "../include/FaceInferencePipeline.h"
#endif

#if __has_include(<opencv2/core.hpp>) && __has_include(<opencv2/imgcodecs.hpp>)
#define RK_CPP_HAS_OPENCV 1
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#else
#define RK_CPP_HAS_OPENCV 0
#endif

#if RK_CPP_HAS_OPENCV
#if __has_include("ArcFaceEmbedder.h")
#include "ArcFaceEmbedder.h"
#else
#include "../include/ArcFaceEmbedder.h"
#endif

#if __has_include("FaceAlign.h")
#include "FaceAlign.h"
#else
#include "../include/FaceAlign.h"
#endif

#if __has_include("FaceSearch.h")
#include "FaceSearch.h"
#else
#include "../include/FaceSearch.h"
#endif

#if __has_include("FaceTemplate.h")
#include "FaceTemplate.h"
#else
#include "../include/FaceTemplate.h"
#endif

#if __has_include("ThresholdPolicy.h")
#include "ThresholdPolicy.h"
#else
#include "../include/ThresholdPolicy.h"
#endif

#if __has_include("YoloFaceDetector.h")
#include "YoloFaceDetector.h"
#else
#include "../include/YoloFaceDetector.h"
#endif

#endif

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

static long long nowEpochMillis() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            static const char kHex[] = "0123456789abcdef";
            out += "\\u00";
            out.push_back(kHex[(c >> 4) & 0x0f]);
            out.push_back(kHex[c & 0x0f]);
        }
        else if (c == 0x7f) {
            out += "\\u007f";
        }
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

class JsonWriter {
public:
    void beginObject() {
        beginValue_();
        oss_ << "{";
        ctx_.push_back(CtxType::Object);
        first_.push_back(true);
    }
    void endObject() {
        oss_ << "}";
        if (!ctx_.empty()) ctx_.pop_back();
        if (!first_.empty()) first_.pop_back();
    }

    void beginArray() {
        beginValue_();
        oss_ << "[";
        ctx_.push_back(CtxType::Array);
        first_.push_back(true);
    }
    void endArray() {
        oss_ << "]";
        if (!ctx_.empty()) ctx_.pop_back();
        if (!first_.empty()) first_.pop_back();
    }

    void key(const char* k) {
        if (!first_.empty()) {
            if (!first_.back()) oss_ << ",";
            first_.back() = false;
        }
        oss_ << "\"" << jsonEscape(k) << "\":";
    }

    void string(const std::string& v) {
        beginValue_();
        oss_ << "\"" << jsonEscape(v) << "\"";
    }
    void string(const char* v) {
        beginValue_();
        oss_ << "\"" << jsonEscape(v ? std::string(v) : std::string()) << "\"";
    }
    void boolean(bool v) {
        beginValue_();
        oss_ << (v ? "true" : "false");
    }
    void number(double v) {
        beginValue_();
        oss_ << v;
    }
    void number(long long v) {
        beginValue_();
        oss_ << v;
    }
    void number(std::uint64_t v) {
        beginValue_();
        oss_ << v;
    }

    std::string str() const { return oss_.str(); }

private:
    enum class CtxType { Object, Array };

    void beginValue_() {
        if (ctx_.empty() || first_.empty()) return;
        if (ctx_.size() != first_.size()) return;
        if (ctx_.back() == CtxType::Array) {
            if (!first_.back()) oss_ << ",";
            first_.back() = false;
        }
    }

    std::ostringstream oss_;
    std::vector<CtxType> ctx_;
    std::vector<bool> first_;
};

#if RK_CPP_HAS_OPENCV
// Optimized to use float instead of double to maximize SIMD vectorization
// and improve execution speed on the RK3288 (ARM Cortex-A17) platform.
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

static std::vector<float> makeFakeEmbedding512(const cv::Rect2f& bbox, const cv::Size& imgSz) {
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

struct FaceInferMetrics {
    long long msLoad = 0;
    long long msDetect = 0;
    long long msAlign = 0;
    long long msEmbed = 0;
    long long msSearch = 0;
    long long msTotal = 0;
};

struct FaceInferContext {
    cv::Mat img;

    FaceDetections faces;
    bool hasFace = false;
    FaceDetection mainFace;

    cv::Mat aligned112;
    bool usedKeypoints5 = false;
    bool usedBboxFallback = false;

    std::vector<float> embedding;
    bool embeddingOk = false;
    bool embeddingFake = false;

    std::string yoloBackendName;
    std::string arcBackendName;

    std::vector<FaceSearchEntry> galleryEntries;
    std::vector<std::string> galleryWarnings;

    FaceSearchLinearIndex index;
    std::vector<FaceSearchHit> hits;
    float bestScore = -1.0f;
    bool hasCandidate = false;
};

static void writeRequestJson(JsonWriter& j, const FaceInferRequest& req) {
    j.key("request");
    j.beginObject();
    j.key("yolo");
    j.beginObject();
    j.key("backend");
    j.string(req.yoloBackend);
    j.key("modelPath");
    j.string(req.yoloModelPath);
    j.key("configPath");
    j.string(req.yoloConfigPath);
    j.key("framework");
    j.string(req.yoloFramework);
    j.key("outputName");
    j.string(req.yoloOutputName);
    j.key("inputW");
    j.number(static_cast<long long>(req.yoloInputW));
    j.key("inputH");
    j.number(static_cast<long long>(req.yoloInputH));
    j.key("scoreThreshold");
    j.number(static_cast<double>(req.yoloScoreThreshold));
    j.key("nmsIouThreshold");
    j.number(static_cast<double>(req.yoloNmsIouThreshold));
    j.key("enableKeypoints5");
    j.boolean(req.yoloEnableKeypoints5);
    j.key("letterbox");
    j.boolean(req.yoloLetterbox);
    j.key("swapRB");
    j.boolean(req.yoloSwapRB);
    j.key("scale");
    j.number(static_cast<double>(req.yoloScale));
    j.key("meanB");
    j.number(static_cast<long long>(req.yoloMeanB));
    j.key("meanG");
    j.number(static_cast<long long>(req.yoloMeanG));
    j.key("meanR");
    j.number(static_cast<long long>(req.yoloMeanR));
    j.key("opencvBackend");
    j.number(static_cast<long long>(req.yoloOpenCvBackend));
    j.key("opencvTarget");
    j.number(static_cast<long long>(req.yoloOpenCvTarget));
    j.endObject();
    j.key("arc");
    j.beginObject();
    j.key("backend");
    j.string(req.arcBackend);
    j.key("modelPath");
    j.string(req.arcModelPath);
    j.key("configPath");
    j.string(req.arcConfigPath);
    j.key("framework");
    j.string(req.arcFramework);
    j.key("outputName");
    j.string(req.arcOutputName);
    j.key("inputName");
    j.string(req.arcInputName);
    j.key("inputW");
    j.number(static_cast<long long>(req.arcInputW));
    j.key("inputH");
    j.number(static_cast<long long>(req.arcInputH));
    j.key("modelVersion");
    j.number(static_cast<std::uint64_t>(req.arcModelVersion));
    j.key("preprocessVersion");
    j.number(static_cast<std::uint64_t>(req.arcPreprocessVersion));
    j.key("fakeEmbedding");
    j.boolean(req.fakeEmbedding);
    j.endObject();
    j.key("gallery");
    j.beginObject();
    j.key("dir");
    j.string(req.galleryDir);
    j.key("topK");
    j.number(static_cast<std::uint64_t>(req.topK));
    j.endObject();
    j.key("threshold");
    j.beginObject();
    j.key("acceptThreshold");
    j.number(static_cast<double>(req.acceptThreshold));
    j.key("versionId");
    j.string(req.thresholdVersionId);
    j.key("consecutivePassesToTrigger");
    j.number(static_cast<long long>(req.consecutivePassesToTrigger));
    j.endObject();
    j.key("faceSelectPolicy");
    j.string(req.faceSelectPolicy);
    j.key("fakeDetect");
    j.boolean(req.fakeDetect);
    j.endObject();
}

static std::string buildImageLoadFailureJson(const FaceInferOutcome& out,
                                             const FaceInferRequest& req,
                                             const FaceInferMetrics& m,
                                             long long tsMs) {
    JsonWriter j;
    j.beginObject();
    j.key("ok");
    j.boolean(false);
    j.key("errorCode");
    j.number(static_cast<long long>(out.errorCode));
    j.key("stage");
    j.string(out.stage);
    j.key("message");
    j.string(out.message);
    j.key("timestamp_ms");
    j.number(tsMs);
    j.key("image");
    j.string(req.imagePath);
    j.key("frame");
    j.beginObject();
    j.key("w");
    j.number(0LL);
    j.key("h");
    j.number(0LL);
    j.endObject();
    j.key("metrics");
    j.beginObject();
    j.key("msLoad");
    j.number(m.msLoad);
    j.key("msTotal");
    j.number(m.msTotal);
    j.endObject();
    writeRequestJson(j, req);
    j.endObject();
    return j.str();
}

static std::string buildOutcomeJson(const FaceInferOutcome& out,
                                    const FaceInferRequest& req,
                                    const FaceInferContext& ctx,
                                    const ThresholdDecisionResult& decision,
                                    const FaceInferMetrics& m,
                                    long long tsMs) {
    JsonWriter j;
    j.beginObject();
    j.key("ok");
    j.boolean(out.ok);
    j.key("errorCode");
    j.number(static_cast<long long>(out.errorCode));
    j.key("stage");
    j.string(out.stage);
    j.key("message");
    j.string(out.message);
    j.key("timestamp_ms");
    j.number(tsMs);
    j.key("image");
    j.string(req.imagePath);
    j.key("modelVersion");
    j.number(static_cast<std::uint64_t>(req.arcModelVersion));
    j.key("thresholdVersion");
    j.string(decision.versionId);
    j.key("frame");
    j.beginObject();
    j.key("w");
    j.number(static_cast<long long>(ctx.img.cols));
    j.key("h");
    j.number(static_cast<long long>(ctx.img.rows));
    j.endObject();
    j.key("yolo");
    j.beginObject();
    j.key("backend");
    j.string(ctx.yoloBackendName.empty() ? req.yoloBackend : ctx.yoloBackendName);
    j.key("fake");
    j.boolean(req.fakeDetect);
    j.endObject();
    j.key("face");
    j.beginObject();
    j.key("hasFace");
    j.boolean(ctx.hasFace);
    if (ctx.hasFace) {
        j.key("bbox");
        j.beginObject();
        j.key("x");
        j.number(static_cast<double>(ctx.mainFace.bbox.x));
        j.key("y");
        j.number(static_cast<double>(ctx.mainFace.bbox.y));
        j.key("w");
        j.number(static_cast<double>(ctx.mainFace.bbox.width));
        j.key("h");
        j.number(static_cast<double>(ctx.mainFace.bbox.height));
        j.endObject();
        j.key("score");
        j.number(static_cast<double>(ctx.mainFace.score));
        j.key("hasKeypoints5");
        j.boolean(ctx.mainFace.keypoints5.has_value());
        j.key("align");
        j.beginObject();
        j.key("usedKeypoints5");
        j.boolean(ctx.usedKeypoints5);
        j.key("usedBboxFallback");
        j.boolean(ctx.usedBboxFallback);
        j.key("outW");
        j.number(static_cast<long long>(req.arcInputW));
        j.key("outH");
        j.number(static_cast<long long>(req.arcInputH));
        j.endObject();
    }
    j.endObject();
    j.key("embedding");
    j.beginObject();
    j.key("dim");
    j.number(512LL);
    j.key("backend");
    j.string(ctx.arcBackendName);
    j.key("fake");
    j.boolean(ctx.embeddingFake);
    j.key("modelVersion");
    j.number(static_cast<std::uint64_t>(req.arcModelVersion));
    j.key("preprocessVersion");
    j.number(static_cast<std::uint64_t>(req.arcPreprocessVersion));
    j.endObject();
    j.key("gallery");
    j.beginObject();
    j.key("dir");
    j.string(req.galleryDir);
    j.key("size");
    j.number(static_cast<std::uint64_t>((ctx.hasFace && ctx.embeddingOk) ? ctx.index.size() : 0));
    if (!ctx.galleryWarnings.empty()) {
        j.key("warnings");
        j.beginArray();
        for (const auto& w : ctx.galleryWarnings) j.string(w);
        j.endArray();
    }
    j.endObject();
    j.key("TopK");
    j.beginObject();
    j.key("k");
    j.number(static_cast<std::uint64_t>(req.topK));
    j.key("hits");
    j.beginArray();
    for (size_t i = 0; i < ctx.hits.size(); i++) {
        j.beginObject();
        j.key("rank");
        j.number(static_cast<long long>(i + 1));
        j.key("id");
        j.string(ctx.hits[i].id);
        j.key("index");
        j.number(static_cast<long long>(ctx.hits[i].index));
        j.key("score");
        j.number(static_cast<double>(ctx.hits[i].score));
        j.endObject();
    }
    j.endArray();
    j.endObject();
    j.key("decision");
    j.beginObject();
    j.key("threshold");
    j.number(static_cast<double>(decision.threshold));
    j.key("thresholdVersion");
    j.string(decision.versionId);
    j.key("passNow");
    j.boolean(decision.passNow);
    j.key("triggeredNow");
    j.boolean(decision.triggeredNow);
    j.key("triggeredLatched");
    j.boolean(decision.triggeredLatched);
    j.key("passStreak");
    j.number(static_cast<std::uint64_t>(decision.passStreak));
    j.key("bestScore");
    j.number(static_cast<double>(decision.score));
    j.endObject();
    j.key("metrics");
    j.beginObject();
    j.key("msLoad");
    j.number(m.msLoad);
    j.key("msDetect");
    j.number(m.msDetect);
    j.key("msAlign");
    j.number(m.msAlign);
    j.key("msEmbed");
    j.number(m.msEmbed);
    j.key("msSearch");
    j.number(m.msSearch);
    j.key("msTotal");
    j.number(m.msTotal);
    j.endObject();
    j.endObject();
    return j.str();
}

static size_t pickMainFaceIndex(const FaceInferRequest& req, const FaceDetections& faces) {
    if (faces.empty()) return 0;
    std::string p;
    p.reserve(req.faceSelectPolicy.size());
    for (char c : req.faceSelectPolicy) p.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

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

static int errorCodeForStage(const std::string& stage) {
    if (stage == "image_load") return 100;
    if (stage == "yolo_load") return 200;
    if (stage == "yolo_detect") return 210;
    if (stage == "align") return 300;
    if (stage == "arc_init") return 400;
    if (stage == "arc_embed") return 410;
    if (stage == "gallery_load") return 500;
    if (stage == "search") return 510;
    if (stage == "exception") return 900;
    return 1;
}

#else
static int errorCodeForStage(const std::string& stage) {
    if (stage == "image_load") return 100;
    if (stage == "opencv_headers") return 110;
    if (stage == "yolo_load") return 200;
    if (stage == "yolo_detect") return 210;
    if (stage == "align") return 300;
    if (stage == "arc_init") return 400;
    if (stage == "arc_embed") return 410;
    if (stage == "gallery_load") return 500;
    if (stage == "search") return 510;
    if (stage == "exception") return 900;
    return 1;
}
#endif

}  // namespace

#if RK_CPP_HAS_OPENCV
FaceInferOutcome runFaceInferOnce(const FaceInferRequest& req) {
    FaceInferOutcome out;
    const long long tsMs = nowEpochMillis();
    try {
        const auto t0 = std::chrono::steady_clock::now();
        FaceInferContext ctx;
        FaceInferMetrics m;
        ctx.arcBackendName = req.arcBackend;

        ctx.img = cv::imread(req.imagePath, cv::IMREAD_COLOR);
        const auto t1 = std::chrono::steady_clock::now();
        m.msLoad = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        std::string stage;
        std::string msg;

        do {
            if (ctx.img.empty()) {
                stage = "image_load";
                msg = "image_load_failed";
                break;
            }

            if (req.fakeDetect) {
                ctx.faces = fakeDetectCenterFace(ctx.img);
                ctx.yoloBackendName = "fake";
            } else {
                std::string detErr;
                std::unique_ptr<YoloFaceDetector> det;
                if (req.yoloBackend == "opencv" || req.yoloBackend == "opencv_dnn") {
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
                    if (!det->load(spec, opt, detErr)) {
                        stage = "yolo_load";
                        msg = detErr.empty() ? "yolo_load_failed" : detErr;
                        break;
                    }
                } else if (req.yoloBackend == "ncnn") {
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
                    stage = "yolo_load";
                    msg = "ncnn_backend_requires_param_bin_via_yolo_face_cli";
#else
                    stage = "yolo_load";
                    msg = "RK_HAVE_NCNN_not_enabled";
#endif
                    break;
                } else if (req.yoloBackend == "qualcomm") {
                    // [Qualcomm SDK Placeholder]
                    rklog::logInfo("FaceInferencePipeline", "yoloBackend", "Qualcomm SDK fallback to CPU... 待补测");
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
                        stage = "yolo_load";
                        msg = loadErr.empty() ? "yolo_load_failed" : loadErr;
                        break;
                    }
                } else {
                    stage = "yolo_load";
                    msg = "yolo_backend_unsupported";
                    break;
                }

                if (det) {
                    ctx.yoloBackendName = det->backendName();
                    const auto td0 = std::chrono::steady_clock::now();
                    ctx.faces = det->detect(ctx.img, detErr);
                    const auto td1 = std::chrono::steady_clock::now();
                    m.msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count();
                    if (!detErr.empty()) {
                        stage = "yolo_detect";
                        msg = detErr;
                        break;
                    }
                }
            }

            if (!ctx.faces.empty()) {
                ctx.hasFace = true;
                const size_t bestIdx = pickMainFaceIndex(req, ctx.faces);
                ctx.mainFace = ctx.faces[bestIdx];
            }

            if (ctx.hasFace) {
                const auto ta0 = std::chrono::steady_clock::now();
                FaceAlignOptions aopt;
                aopt.outW = req.arcInputW;
                aopt.outH = req.arcInputH;
                aopt.preferKeypoints5 = true;
                auto ar = alignFaceForArcFace112(ctx.img, ctx.mainFace, aopt);
                const auto ta1 = std::chrono::steady_clock::now();
                m.msAlign = std::chrono::duration_cast<std::chrono::milliseconds>(ta1 - ta0).count();
                if (ar.alignedBgr.empty()) {
                    stage = "align";
                    msg = ar.err.empty() ? "align_failed" : ar.err;
                    break;
                }
                ctx.aligned112 = std::move(ar.alignedBgr);
                ctx.usedKeypoints5 = ar.usedKeypoints5;
                ctx.usedBboxFallback = ar.usedBboxFallback;
            }

            if (ctx.hasFace) {
                const auto te0 = std::chrono::steady_clock::now();
                if (req.fakeEmbedding) {
                    ctx.embedding = makeFakeEmbedding512(ctx.mainFace.bbox, ctx.img.size());
                    ctx.embeddingOk = (ctx.embedding.size() == static_cast<size_t>(ArcFaceEmbedding::kDim));
                    ctx.embeddingFake = true;
                } else {
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
                    } else if (req.arcBackend == "qualcomm") {
                        cfg.backend = ArcFaceEmbedderConfig::BackendType::Qualcomm;
                        cfg.qualcommModel = req.arcModelPath;
                    } else {
                        stage = "arc_init";
                        msg = "arc_backend_unsupported";
                        break;
                    }

                    ArcFaceEmbedder emb;
                    std::string initErr;
                    if (!emb.initialize(cfg, &initErr)) {
                        stage = "arc_init";
                        msg = initErr.empty() ? "arc_init_failed" : initErr;
                        break;
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
                        stage = "arc_embed";
                        msg = embedErr.empty() ? "arc_embed_failed" : embedErr;
                        break;
                    }
                    ctx.embedding = std::move(e->values);
                    ctx.embeddingOk = (ctx.embedding.size() == static_cast<size_t>(ArcFaceEmbedding::kDim));
                }
                const auto te1 = std::chrono::steady_clock::now();
                m.msEmbed = std::chrono::duration_cast<std::chrono::milliseconds>(te1 - te0).count();
            }

            std::string galleryErr;
            if (!loadGalleryDir(req.galleryDir, ctx.galleryEntries, ctx.galleryWarnings, galleryErr)) {
                ctx.galleryEntries.clear();
                stage = "gallery_load";
                msg = galleryErr.empty() ? "gallery_load_failed" : galleryErr;
                break;
            }

            if (ctx.hasFace && ctx.embeddingOk) {
                std::string searchErr;
                const auto ts0 = std::chrono::steady_clock::now();
                if (!ctx.index.reset(std::move(ctx.galleryEntries), static_cast<size_t>(ArcFaceEmbedding::kDim), searchErr)) {
                    stage = "search";
                    msg = searchErr.empty() ? "index_reset_failed" : searchErr;
                    const auto ts1 = std::chrono::steady_clock::now();
                    m.msSearch = std::chrono::duration_cast<std::chrono::milliseconds>(ts1 - ts0).count();
                    ctx.hits.clear();
                    ctx.hasCandidate = false;
                    ctx.bestScore = -1.0f;
                    break;
                } else if (ctx.index.size() > 0) {
                    FaceSearchOptions opt;
                    opt.assumeL2Normalized = true;
                    ctx.hits = ctx.index.searchTopK(ctx.embedding, req.topK, opt, searchErr);
                    if (!searchErr.empty()) {
                        stage = "search";
                        msg = searchErr;
                    }
                }
                const auto ts1 = std::chrono::steady_clock::now();
                m.msSearch = std::chrono::duration_cast<std::chrono::milliseconds>(ts1 - ts0).count();
                if (!ctx.hits.empty()) {
                    ctx.bestScore = ctx.hits[0].score;
                    ctx.hasCandidate = true;
                }
                if (!stage.empty()) break;
            }
        } while (false);

        if (stage == "image_load") {
            m.msTotal = m.msLoad;
            out.ok = false;
            out.stage = "image_load";
            out.message = msg;
            out.errorCode = errorCodeForStage(out.stage);
            out.auditDir = "ErrorLog";
            out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";
            out.json = buildImageLoadFailureJson(out, req, m, tsMs);
            return out;
        }

        ThresholdPolicyVersion v;
        v.versionId = req.thresholdVersionId;
        v.acceptThreshold = req.acceptThreshold;
        v.consecutivePassesToTrigger =
            req.consecutivePassesToTrigger > 0 ? static_cast<size_t>(req.consecutivePassesToTrigger) : 1;
        ThresholdDecisionPolicy policy(v);
        const ThresholdDecisionResult decision = policy.feed(ctx.bestScore, ctx.hasCandidate);

        const auto tEnd = std::chrono::steady_clock::now();
        m.msTotal = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - t0).count();

        out.ok = stage.empty();
        out.stage = stage.empty() ? "done" : stage;
        out.message = stage.empty() ? (ctx.hasFace ? "" : "no_face_detected") : msg;
        out.errorCode = stage.empty() ? 0 : errorCodeForStage(stage);
        out.auditDir = out.ok ? "tests/metrics" : "ErrorLog";
        out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";

        out.json = buildOutcomeJson(out, req, ctx, decision, m, tsMs);
        return out;
    } catch (const std::exception& e) {
        out.ok = false;
        out.stage = "exception";
        out.message = e.what();
        out.errorCode = errorCodeForStage(out.stage);
        out.auditDir = "ErrorLog";
        out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";
        JsonWriter j;
        j.beginObject();
        j.key("ok");
        j.boolean(false);
        j.key("errorCode");
        j.number(static_cast<long long>(out.errorCode));
        j.key("stage");
        j.string("exception");
        j.key("message");
        j.string(out.message);
        j.key("timestamp_ms");
        j.number(tsMs);
        j.key("image");
        j.string(req.imagePath);
        j.endObject();
        out.json = j.str();
        return out;
    } catch (...) {
        out.ok = false;
        out.stage = "exception";
        out.message = "unknown_exception";
        out.errorCode = errorCodeForStage(out.stage);
        out.auditDir = "ErrorLog";
        out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";
        JsonWriter j;
        j.beginObject();
        j.key("ok");
        j.boolean(false);
        j.key("errorCode");
        j.number(static_cast<long long>(out.errorCode));
        j.key("stage");
        j.string("exception");
        j.key("message");
        j.string("unknown_exception");
        j.key("timestamp_ms");
        j.number(tsMs);
        j.endObject();
        out.json = j.str();
        return out;
    }
}
#else
FaceInferOutcome runFaceInferOnce(const FaceInferRequest& req) {
    FaceInferOutcome out;
    const long long tsMs = nowEpochMillis();
    out.ok = false;
    out.stage = "opencv_headers";
    out.message = "opencv_headers_not_found";
    out.errorCode = errorCodeForStage(out.stage);
    out.auditDir = "ErrorLog";
    out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";
    JsonWriter j;
    j.beginObject();
    j.key("ok");
    j.boolean(false);
    j.key("errorCode");
    j.number(static_cast<long long>(out.errorCode));
    j.key("stage");
    j.string(out.stage);
    j.key("message");
    j.string(out.message);
    j.key("timestamp_ms");
    j.number(tsMs);
    j.key("image");
    j.string(req.imagePath);
    j.endObject();
    out.json = j.str();
    return out;
}
#endif
