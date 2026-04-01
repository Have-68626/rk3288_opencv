#include "FaceInferencePipeline.h"

#include "ArcFaceEmbedder.h"
#include "FaceAlign.h"
#include "FaceSearch.h"
#include "FaceTemplate.h"
#include "ThresholdPolicy.h"
#include "YoloFaceDetector.h"

#include <opencv2/imgcodecs.hpp>

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
        oss_ << "{";
        first_.push_back(true);
    }
    void endObject() {
        oss_ << "}";
        if (!first_.empty()) first_.pop_back();
    }

    void key(const char* k) {
        if (!first_.empty()) {
            if (!first_.back()) oss_ << ",";
            first_.back() = false;
        }
        oss_ << "\"" << jsonEscape(k) << "\":";
    }

    void string(const std::string& v) { oss_ << "\"" << jsonEscape(v) << "\""; }
    void string(const char* v) { oss_ << "\"" << jsonEscape(v ? std::string(v) : std::string()) << "\""; }
    void boolean(bool v) { oss_ << (v ? "true" : "false"); }
    void number(double v) { oss_ << v; }
    void number(long long v) { oss_ << v; }
    void number(std::uint64_t v) { oss_ << v; }

    std::string str() const { return oss_.str(); }

private:
    std::ostringstream oss_;
    std::vector<bool> first_;
};

static bool l2NormalizeInplace(std::vector<float>& v) {
    if (v.empty()) return false;
    double s = 0.0;
    for (float x : v) s += static_cast<double>(x) * static_cast<double>(x);
    if (!(s > 0.0) || !std::isfinite(s)) return false;
    const double n = std::sqrt(s);
    if (!(n > 0.0) || !std::isfinite(n)) return false;
    const float inv = static_cast<float>(1.0 / n);
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

}  // namespace

FaceInferOutcome runFaceInferOnce(const FaceInferRequest& req) {
    FaceInferOutcome out;
    const long long tsMs = nowEpochMillis();
    try {
        const auto t0 = std::chrono::steady_clock::now();

        cv::Mat img = cv::imread(req.imagePath, cv::IMREAD_COLOR);
        const auto t1 = std::chrono::steady_clock::now();
        const long long msLoad = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        std::string stage;
        std::string msg;

        if (img.empty()) {
            const long long msTotal = msLoad;
            out.ok = false;
            out.stage = "image_load";
            out.message = "image_load_failed";
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
            j.number(msLoad);
            j.key("msTotal");
            j.number(msTotal);
            j.endObject();
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
            j.endObject();
            out.json = j.str();
            return out;
        }

    FaceDetections faces;
    std::string yoloBackendName;
    long long msDetect = 0;

    if (stage.empty()) {
        if (req.fakeDetect) {
            faces = fakeDetectCenterFace(img);
            yoloBackendName = "fake";
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
                }
            } else if (req.yoloBackend == "ncnn") {
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
                stage = "yolo_load";
                msg = "ncnn_backend_requires_param_bin_via_yolo_face_cli";
#else
                stage = "yolo_load";
                msg = "RK_HAVE_NCNN_not_enabled";
#endif
            } else {
                stage = "yolo_load";
                msg = "yolo_backend_unsupported";
            }

            if (stage.empty() && det) {
                yoloBackendName = det->backendName();
                const auto td0 = std::chrono::steady_clock::now();
                faces = det->detect(img, detErr);
                const auto td1 = std::chrono::steady_clock::now();
                msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count();
                if (!detErr.empty()) {
                    stage = "yolo_detect";
                    msg = detErr;
                }
            }
        }
    }

    bool hasFace = false;
    FaceDetection mainFace;
    if (stage.empty() && !faces.empty()) {
        hasFace = true;
        size_t bestIdx = 0;
        std::string p;
        p.reserve(req.faceSelectPolicy.size());
        for (char c : req.faceSelectPolicy) p.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        if (p == "first") {
            bestIdx = 0;
        } else if (p == "area" || p == "largest") {
            for (size_t i = 1; i < faces.size(); i++) {
                const auto& a = faces[i];
                const auto& b = faces[bestIdx];
                const float areaA = a.bbox.width * a.bbox.height;
                const float areaB = b.bbox.width * b.bbox.height;
                if (areaA > areaB) bestIdx = i;
            }
        } else if (p == "score") {
            for (size_t i = 1; i < faces.size(); i++) {
                const auto& a = faces[i];
                const auto& b = faces[bestIdx];
                const float areaA = a.bbox.width * a.bbox.height;
                const float areaB = b.bbox.width * b.bbox.height;
                if (a.score > b.score) bestIdx = i;
                else if (a.score == b.score && areaA > areaB) bestIdx = i;
            }
        } else {
            for (size_t i = 1; i < faces.size(); i++) {
                const auto& a = faces[i];
                const auto& b = faces[bestIdx];
                const float areaA = a.bbox.width * a.bbox.height;
                const float areaB = b.bbox.width * b.bbox.height;
                if (a.score > b.score) bestIdx = i;
                else if (a.score == b.score && areaA > areaB) bestIdx = i;
            }
        }
        mainFace = faces[bestIdx];
    }

    cv::Mat aligned112;
    bool usedKeypoints5 = false;
    bool usedBboxFallback = false;
    long long msAlign = 0;

    if (stage.empty() && hasFace) {
        const auto ta0 = std::chrono::steady_clock::now();
        FaceAlignOptions aopt;
        aopt.outW = req.arcInputW;
        aopt.outH = req.arcInputH;
        aopt.preferKeypoints5 = true;
        auto ar = alignFaceForArcFace112(img, mainFace, aopt);
        const auto ta1 = std::chrono::steady_clock::now();
        msAlign = std::chrono::duration_cast<std::chrono::milliseconds>(ta1 - ta0).count();
        if (ar.alignedBgr.empty()) {
            stage = "align";
            msg = ar.err.empty() ? "align_failed" : ar.err;
        } else {
            aligned112 = std::move(ar.alignedBgr);
            usedKeypoints5 = ar.usedKeypoints5;
            usedBboxFallback = ar.usedBboxFallback;
        }
    }

    std::vector<float> embedding;
    bool embeddingOk = false;
    bool embeddingFake = false;
    long long msEmbed = 0;
    std::string arcBackendName = req.arcBackend;

    if (stage.empty() && hasFace) {
        const auto te0 = std::chrono::steady_clock::now();
        if (req.fakeEmbedding) {
            embedding = makeFakeEmbedding512(mainFace.bbox, img.size());
            embeddingOk = (embedding.size() == static_cast<size_t>(ArcFaceEmbedding::kDim));
            embeddingFake = true;
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
            } else {
                stage = "arc_init";
                msg = "arc_backend_unsupported";
            }

            if (stage.empty()) {
                ArcFaceEmbedder emb;
                std::string initErr;
                if (!emb.initialize(cfg, &initErr)) {
                    stage = "arc_init";
                    msg = initErr.empty() ? "arc_init_failed" : initErr;
                } else {
                    arcBackendName = (cfg.backend == ArcFaceEmbedderConfig::BackendType::OpenCvDnn) ? "opencv_dnn" : "ncnn";
                    std::string embedErr;
                    auto e = emb.embedAlignedFaceBgr(aligned112, &embedErr);
                    if (!e.has_value()) {
                        stage = "arc_embed";
                        msg = embedErr.empty() ? "arc_embed_failed" : embedErr;
                    } else {
                        embedding = std::move(e->values);
                        embeddingOk = (embedding.size() == static_cast<size_t>(ArcFaceEmbedding::kDim));
                    }
                }
            }
        }
        const auto te1 = std::chrono::steady_clock::now();
        msEmbed = std::chrono::duration_cast<std::chrono::milliseconds>(te1 - te0).count();
    }

    std::vector<FaceSearchEntry> galleryEntries;
    std::vector<std::string> galleryWarnings;
    std::string galleryErr;
    if (stage.empty()) {
        if (!loadGalleryDir(req.galleryDir, galleryEntries, galleryWarnings, galleryErr)) {
            stage = "gallery_load";
            msg = galleryErr.empty() ? "gallery_load_failed" : galleryErr;
        }
    }

    FaceSearchLinearIndex index;
    std::vector<FaceSearchHit> hits;
    std::string searchErr;
    long long msSearch = 0;

    float bestScore = -1.0f;
    bool hasCandidate = false;

    if (stage.empty()) {
        if (hasFace && embeddingOk) {
            const auto ts0 = std::chrono::steady_clock::now();
            if (!index.reset(std::move(galleryEntries), static_cast<size_t>(ArcFaceEmbedding::kDim), searchErr)) {
                stage = "search";
                msg = searchErr.empty() ? "index_reset_failed" : searchErr;
            } else {
                if (index.size() > 0) {
                    FaceSearchOptions opt;
                    opt.assumeL2Normalized = true;
                    hits = index.searchTopK(embedding, req.topK, opt, searchErr);
                    if (!searchErr.empty()) {
                        stage = "search";
                        msg = searchErr;
                    }
                }
            }
            const auto ts1 = std::chrono::steady_clock::now();
            msSearch = std::chrono::duration_cast<std::chrono::milliseconds>(ts1 - ts0).count();

            if (stage.empty() && !hits.empty()) {
                bestScore = hits[0].score;
                hasCandidate = true;
            }
        }
    }

    ThresholdPolicyVersion v;
    v.versionId = req.thresholdVersionId;
    v.acceptThreshold = req.acceptThreshold;
    v.consecutivePassesToTrigger = req.consecutivePassesToTrigger > 0 ? static_cast<size_t>(req.consecutivePassesToTrigger) : 1;
    ThresholdDecisionPolicy policy(v);
    const ThresholdDecisionResult decision = policy.feed(bestScore, hasCandidate);

        const auto tEnd = std::chrono::steady_clock::now();
        const long long msTotal = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - t0).count();

        out.ok = stage.empty();
        out.stage = stage.empty() ? "done" : stage;
        if (!stage.empty()) out.message = msg;
        else out.message = hasFace ? "" : "no_face_detected";
        out.errorCode = stage.empty() ? 0 : errorCodeForStage(stage);
        out.auditDir = out.ok ? "tests/metrics" : "ErrorLog";
        out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";

        std::ostringstream jout;
        jout << "{";
        jout << "\"ok\":" << (out.ok ? "true" : "false") << ",";
        jout << "\"errorCode\":" << out.errorCode << ",";
        jout << "\"stage\":\"" << jsonEscape(out.stage) << "\",";
        jout << "\"message\":\"" << jsonEscape(out.message) << "\",";
        jout << "\"timestamp_ms\":" << tsMs << ",";
        jout << "\"image\":\"" << jsonEscape(req.imagePath) << "\",";
        jout << "\"modelVersion\":" << req.arcModelVersion << ",";
        jout << "\"thresholdVersion\":\"" << jsonEscape(decision.versionId) << "\",";
        jout << "\"frame\":{\"w\":" << img.cols << ",\"h\":" << img.rows << "},";
    jout << "\"yolo\":{\"backend\":\"" << jsonEscape(yoloBackendName.empty() ? req.yoloBackend : yoloBackendName) << "\",\"fake\":" << (req.fakeDetect ? "true" : "false") << "},";
    jout << "\"face\":{\"hasFace\":" << (hasFace ? "true" : "false");
    if (hasFace) {
        jout << ",\"bbox\":{\"x\":" << mainFace.bbox.x << ",\"y\":" << mainFace.bbox.y << ",\"w\":" << mainFace.bbox.width << ",\"h\":" << mainFace.bbox.height << "}";
        jout << ",\"score\":" << mainFace.score;
        jout << ",\"hasKeypoints5\":" << (mainFace.keypoints5.has_value() ? "true" : "false");
        jout << ",\"align\":{\"usedKeypoints5\":" << (usedKeypoints5 ? "true" : "false") << ",\"usedBboxFallback\":" << (usedBboxFallback ? "true" : "false")
             << ",\"outW\":" << req.arcInputW << ",\"outH\":" << req.arcInputH << "}";
    }
    jout << "},";
    jout << "\"embedding\":{\"dim\":512,\"backend\":\"" << jsonEscape(arcBackendName) << "\",\"fake\":" << (embeddingFake ? "true" : "false")
         << ",\"modelVersion\":" << req.arcModelVersion << ",\"preprocessVersion\":" << req.arcPreprocessVersion << "},";
    jout << "\"gallery\":{\"dir\":\"" << jsonEscape(req.galleryDir) << "\",\"size\":" << (hasFace && embeddingOk ? index.size() : 0);
    if (!galleryWarnings.empty()) {
        jout << ",\"warnings\":[";
        for (size_t i = 0; i < galleryWarnings.size(); i++) {
            if (i) jout << ",";
            jout << "\"" << jsonEscape(galleryWarnings[i]) << "\"";
        }
        jout << "]";
    }
    jout << "},";
    jout << "\"TopK\":{";
    jout << "\"k\":" << req.topK << ",";
    jout << "\"hits\":[";
    for (size_t i = 0; i < hits.size(); i++) {
        if (i) jout << ",";
        jout << "{";
        jout << "\"rank\":" << (i + 1) << ",";
        jout << "\"id\":\"" << jsonEscape(hits[i].id) << "\",";
        jout << "\"index\":" << hits[i].index << ",";
        jout << "\"score\":" << hits[i].score;
        jout << "}";
    }
    jout << "]";
    jout << "},";
    jout << "\"decision\":{";
    jout << "\"threshold\":" << decision.threshold << ",";
    jout << "\"thresholdVersion\":\"" << jsonEscape(decision.versionId) << "\",";
    jout << "\"passNow\":" << (decision.passNow ? "true" : "false") << ",";
    jout << "\"triggeredNow\":" << (decision.triggeredNow ? "true" : "false") << ",";
    jout << "\"triggeredLatched\":" << (decision.triggeredLatched ? "true" : "false") << ",";
    jout << "\"passStreak\":" << decision.passStreak << ",";
    jout << "\"bestScore\":" << decision.score;
    jout << "},";
    jout << "\"metrics\":{\"msLoad\":" << msLoad << ",\"msDetect\":" << msDetect << ",\"msAlign\":" << msAlign << ",\"msEmbed\":" << msEmbed
         << ",\"msSearch\":" << msSearch << ",\"msTotal\":" << msTotal << "}";
    jout << "}";

        out.json = jout.str();
        return out;
    } catch (const std::exception& e) {
        out.ok = false;
        out.stage = "exception";
        out.message = e.what();
        out.errorCode = errorCodeForStage(out.stage);
        out.auditDir = "ErrorLog";
        out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";
        std::ostringstream jout;
        jout << "{";
        jout << "\"ok\":false,";
        jout << "\"errorCode\":" << out.errorCode << ",";
        jout << "\"stage\":\"exception\",";
        jout << "\"message\":\"" << jsonEscape(out.message) << "\",";
        jout << "\"timestamp_ms\":" << tsMs << ",";
        jout << "\"image\":\"" << jsonEscape(req.imagePath) << "\"";
        jout << "}";
        out.json = jout.str();
        return out;
    } catch (...) {
        out.ok = false;
        out.stage = "exception";
        out.message = "unknown_exception";
        out.errorCode = errorCodeForStage(out.stage);
        out.auditDir = "ErrorLog";
        out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";
        out.json = std::string("{\"ok\":false,\"errorCode\":") + std::to_string(out.errorCode) +
                   ",\"stage\":\"exception\",\"message\":\"unknown_exception\",\"timestamp_ms\":" + std::to_string(tsMs) + "}";
        return out;
    }
}
