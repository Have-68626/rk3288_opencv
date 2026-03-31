/**
 * @file main.cpp
 * @brief Application entry point.
 * 
 * Initializes the AI Engine and starts the main processing loop.
 * Handles signal interruption for graceful shutdown.
 */
#include "Engine.h"
#include "ArcFaceEmbedder.h"
#include "FaceAlign.h"
#include "FaceSearch.h"
#include "FaceInferencePipeline.h"
#include "FaceTemplate.h"
#include "ThresholdPolicy.h"
#include "Storage.h"
#include "YoloFaceDetector.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/features2d.hpp>
#include <cctype>
#include <fstream>
#include <iostream>
#include <csignal>
#include <atomic>
#include <sstream>
#include <string>
#include <chrono>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

// Global pointer for signal handler
std::atomic<Engine*> g_engine(nullptr);

void signalHandler(int signum) {
    std::cout << "\nInterrupt signal (" << signum << ") received." << std::endl;
    Engine* engine = g_engine.load();
    if (engine) {
        engine->stop();
    }
}

static size_t getRssBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<size_t>(pmc.WorkingSetSize);
    }
    return 0;
#else
    return 0;
#endif
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) out += "?";
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

static bool isFlagTrue(const std::string& v) {
    std::string s;
    s.reserve(v.size());
    for (char c : v) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

static long long nowEpochMillis() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

static int runAnalyze(const std::string& imagePath, const std::string& cascadePath, const std::string& outDir) {
    const auto t0 = std::chrono::steady_clock::now();

    cv::Mat img = cv::imread(imagePath, cv::IMREAD_COLOR);
    if (img.empty()) {
        std::cerr << "ANALYZE_ERROR image_load_failed path=" << imagePath << std::endl;
        return 2;
    }

    cv::CascadeClassifier cc;
    if (!cc.load(cascadePath)) {
        std::cerr << "ANALYZE_ERROR cascade_load_failed path=" << cascadePath << std::endl;
        return 3;
    }

    cv::Mat gray;
    cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    cv::equalizeHist(gray, gray);

    const auto t1 = std::chrono::steady_clock::now();
    std::vector<cv::Rect> faces;
    cc.detectMultiScale(gray, faces, 1.1, 3, 0, cv::Size(30, 30));
    const auto t2 = std::chrono::steady_clock::now();

    std::vector<cv::KeyPoint> kps;
    cv::Mat desc;
    auto orb = cv::ORB::create(1000);
    orb->detectAndCompute(gray, cv::noArray(), kps, desc);
    const auto t3 = std::chrono::steady_clock::now();

    if (!outDir.empty()) {
        Storage::ensureDirectory(outDir);
        for (const auto& r : faces) {
            cv::rectangle(img, r, cv::Scalar(0, 255, 0), 2);
        }
        std::filesystem::path p(imagePath);
        std::string outPath = (std::filesystem::path(outDir) / ("annotated_" + p.filename().string() + ".jpg")).string();
        Storage::saveImage(outPath, img);
    }

    const auto msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    const auto msOrb = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
    const auto msTotal = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t0).count();
    const auto rssBytes = getRssBytes();

    std::cout
        << "ANALYZE_OK"
        << " path=" << imagePath
        << " width=" << img.cols
        << " height=" << img.rows
        << " faces=" << faces.size()
        << " keypoints=" << kps.size()
        << " ms_detect=" << msDetect
        << " ms_orb=" << msOrb
        << " ms_total=" << msTotal
        << " rss_bytes=" << rssBytes
        << std::endl;

    return 0;
}

struct YoloFaceCliArgs {
    std::string imagePath;
    std::string backend = "opencv";

    std::string modelPath;
    std::string configPath;
    std::string framework;
    std::string outputName;

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    std::string ncnnParam;
    std::string ncnnBin;
    std::string ncnnInput = "data";
    std::string ncnnOutput = "output";
    int ncnnThreads = 1;
    bool ncnnLightmode = true;
#endif

    YoloFaceOptions opt;
    std::string outDir = "tests/metrics";
    std::string outPrefix = "yolo_face_detect";
};

static int runYoloFaceDetect(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: rk3288_cli --yolo-face <imagePath> --model <model.onnx> [--backend opencv|ncnn] [--out-dir <dir>] [--out-prefix <name>]" << std::endl;
        return 2;
    }

    YoloFaceCliArgs a;
    a.imagePath = argv[2];

    for (int i = 3; i < argc; i++) {
        const std::string k = argv[i];
        auto nextStr = [&](std::string& v) {
            if (i + 1 < argc) v = argv[++i];
        };
        auto nextInt = [&](int& v) {
            if (i + 1 < argc) v = std::stoi(argv[++i]);
        };
        auto nextFloat = [&](float& v) {
            if (i + 1 < argc) v = std::stof(argv[++i]);
        };
        auto nextBool = [&](bool& v) {
            if (i + 1 < argc) v = isFlagTrue(argv[++i]);
        };

        if (k == "--backend") nextStr(a.backend);
        else if (k == "--model") nextStr(a.modelPath);
        else if (k == "--config") nextStr(a.configPath);
        else if (k == "--framework") nextStr(a.framework);
        else if (k == "--output-name") nextStr(a.outputName);
        else if (k == "--w") nextInt(a.opt.inputW);
        else if (k == "--h") nextInt(a.opt.inputH);
        else if (k == "--score") nextFloat(a.opt.scoreThreshold);
        else if (k == "--nms") nextFloat(a.opt.nmsIouThreshold);
        else if (k == "--kps5") nextBool(a.opt.enableKeypoints5);
        else if (k == "--letterbox") nextBool(a.opt.letterbox);
        else if (k == "--swap-rb") nextBool(a.opt.swapRB);
        else if (k == "--scale") nextFloat(a.opt.scale);
        else if (k == "--mean-b") nextInt(a.opt.meanB);
        else if (k == "--mean-g") nextInt(a.opt.meanG);
        else if (k == "--mean-r") nextInt(a.opt.meanR);
        else if (k == "--opencv-backend") nextInt(a.opt.opencvBackend);
        else if (k == "--opencv-target") nextInt(a.opt.opencvTarget);
        else if (k == "--out-dir") nextStr(a.outDir);
        else if (k == "--out-prefix") nextStr(a.outPrefix);
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
        else if (k == "--ncnn-param") nextStr(a.ncnnParam);
        else if (k == "--ncnn-bin") nextStr(a.ncnnBin);
        else if (k == "--ncnn-input") nextStr(a.ncnnInput);
        else if (k == "--ncnn-output") nextStr(a.ncnnOutput);
        else if (k == "--ncnn-threads") nextInt(a.ncnnThreads);
        else if (k == "--ncnn-lightmode") nextBool(a.ncnnLightmode);
#endif
    }

    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    cv::Mat img = cv::imread(a.imagePath, cv::IMREAD_COLOR);
    const auto t1 = std::chrono::steady_clock::now();
    const long long msLoad = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    bool ok = true;
    FaceDetections faces;
    std::string backendName;
    long long msDetect = 0;

    if (img.empty()) {
        ok = false;
        err = "image_load_failed";
    } else {
        std::unique_ptr<YoloFaceDetector> det;
        if (a.backend == "opencv" || a.backend == "opencv_dnn") {
            det = CreateOpenCvDnnYoloFaceDetector();
            YoloFaceModelSpec spec;
            spec.modelPath = a.modelPath;
            spec.configPath = a.configPath;
            spec.framework = a.framework;
            spec.outputName = a.outputName;
            if (!det->load(spec, a.opt, err)) ok = false;
        } else if (a.backend == "ncnn") {
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
            NcnnYoloFaceModelSpec ns;
            ns.paramPath = a.ncnnParam;
            ns.binPath = a.ncnnBin;
            ns.inputName = a.ncnnInput;
            ns.outputName = a.ncnnOutput;
            ns.threads = a.ncnnThreads;
            ns.lightmode = a.ncnnLightmode;
            det = CreateNcnnYoloFaceDetector(ns);
            YoloFaceModelSpec dummy;
            if (!det->load(dummy, a.opt, err)) ok = false;
#else
            ok = false;
            err = "RK_HAVE_NCNN_not_enabled";
#endif
        } else {
            ok = false;
            err = "backend_unsupported";
        }

        if (ok && det) {
            backendName = det->backendName();
            const auto t2 = std::chrono::steady_clock::now();
            faces = det->detect(img, err);
            const auto t3 = std::chrono::steady_clock::now();
            msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
            if (!err.empty()) ok = false;
        }
    }

    const auto tEnd = std::chrono::steady_clock::now();
    const long long msTotal = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - t0).count();
    const long long tsMs = nowEpochMillis();

    std::ostringstream jout;
    jout << "{";
    jout << "\"ok\":" << (ok ? "true" : "false") << ",";
    jout << "\"backend\":\"" << jsonEscape(backendName.empty() ? a.backend : backendName) << "\",";
    jout << "\"image\":\"" << jsonEscape(a.imagePath) << "\",";
    jout << "\"width\":" << img.cols << ",";
    jout << "\"height\":" << img.rows << ",";
    jout << "\"input_w\":" << a.opt.inputW << ",";
    jout << "\"input_h\":" << a.opt.inputH << ",";
    jout << "\"score_threshold\":" << a.opt.scoreThreshold << ",";
    jout << "\"nms_iou_threshold\":" << a.opt.nmsIouThreshold << ",";
    jout << "\"enable_keypoints5\":" << (a.opt.enableKeypoints5 ? "true" : "false") << ",";
    jout << "\"ms_load\":" << msLoad << ",";
    jout << "\"ms_detect\":" << msDetect << ",";
    jout << "\"ms_total\":" << msTotal << ",";
    jout << "\"timestamp_ms\":" << tsMs << ",";
    jout << "\"faces\":[";
    for (size_t i = 0; i < faces.size(); i++) {
        if (i > 0) jout << ",";
        const auto& f = faces[i];
        jout << "{";
        jout << "\"bbox\":{";
        jout << "\"x\":" << f.bbox.x << ",";
        jout << "\"y\":" << f.bbox.y << ",";
        jout << "\"w\":" << f.bbox.width << ",";
        jout << "\"h\":" << f.bbox.height;
        jout << "},";
        jout << "\"score\":" << f.score;
        if (f.keypoints5.has_value()) {
            jout << ",\"keypoints5\":[";
            const auto& kps = *f.keypoints5;
            for (size_t j = 0; j < kps.size(); j++) {
                if (j > 0) jout << ",";
                jout << "{\"x\":" << kps[j].x << ",\"y\":" << kps[j].y << "}";
            }
            jout << "]";
        }
        jout << "}";
    }
    jout << "],";
    jout << "\"err\":\"" << jsonEscape(err) << "\"";
    jout << "}";

    const std::string json = jout.str();
    std::cout << json << std::endl;

    std::filesystem::path dir = ok ? std::filesystem::path(a.outDir) : std::filesystem::path("ErrorLog");
    Storage::ensureDirectory(dir.string());
    const std::string filename = a.outPrefix + "_" + std::to_string(tsMs) + ".json";
    const std::filesystem::path outPath = dir / filename;
    {
        std::ofstream f(outPath, std::ios::out | std::ios::trunc);
        if (f.is_open()) f << json << std::endl;
    }

    return ok ? 0 : 1;
}

struct FaceInferCliArgs {
    FaceInferRequest req;
};

static int runFaceInferCli(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: rk3288_cli --face-infer <imagePath> "
                     "[--yolo-backend opencv] [--yolo-model <path>] [--yolo-config <path>] [--yolo-framework <name>] [--yolo-output-name <name>] "
                     "[--yolo-w 320] [--yolo-h 320] [--yolo-score 0.25] [--yolo-nms 0.45] [--yolo-kps5 1|0] [--yolo-letterbox 1|0] "
                     "[--yolo-swap-rb 1|0] [--yolo-scale 0.0039215686] [--yolo-mean-b 0] [--yolo-mean-g 0] [--yolo-mean-r 0] "
                     "[--yolo-opencv-backend 0] [--yolo-opencv-target 0] "
                     "[--arc-backend opencv] [--arc-model <path>] [--arc-config <path>] [--arc-framework <name>] [--arc-output-name <name>] [--arc-input-name <name>] [--arc-w 112] [--arc-h 112] "
                     "[--arc-model-version 1] [--arc-preprocess-version 1] "
                     "[--gallery-dir <dir>] [--topk N] [--threshold T] [--threshold-version <id>] [--consecutive N] "
                     "[--face-select score_area|score|area|first] "
                     "[--fake-detect 1|0] [--fake-embedding 1|0]"
                  << std::endl;
        return 2;
    }

    FaceInferCliArgs a;
    a.req.imagePath = argv[2];

    for (int i = 3; i < argc; i++) {
        const std::string k = argv[i];
        auto nextStr = [&](std::string& v) {
            if (i + 1 < argc) v = argv[++i];
        };
        auto nextInt = [&](int& v) {
            if (i + 1 < argc) v = std::stoi(argv[++i]);
        };
        auto nextSizeT = [&](std::size_t& v) {
            if (i + 1 < argc) v = static_cast<std::size_t>(std::stoull(argv[++i]));
        };
        auto nextFloat = [&](float& v) {
            if (i + 1 < argc) v = std::stof(argv[++i]);
        };
        auto nextBool = [&](bool& v) {
            if (i + 1 < argc) v = isFlagTrue(argv[++i]);
        };

        if (k == "--yolo-backend") nextStr(a.req.yoloBackend);
        else if (k == "--yolo-model") nextStr(a.req.yoloModelPath);
        else if (k == "--yolo-config") nextStr(a.req.yoloConfigPath);
        else if (k == "--yolo-framework") nextStr(a.req.yoloFramework);
        else if (k == "--yolo-output-name") nextStr(a.req.yoloOutputName);
        else if (k == "--yolo-w") nextInt(a.req.yoloInputW);
        else if (k == "--yolo-h") nextInt(a.req.yoloInputH);
        else if (k == "--yolo-score") nextFloat(a.req.yoloScoreThreshold);
        else if (k == "--yolo-nms") nextFloat(a.req.yoloNmsIouThreshold);
        else if (k == "--yolo-kps5") nextBool(a.req.yoloEnableKeypoints5);
        else if (k == "--yolo-letterbox") nextBool(a.req.yoloLetterbox);
        else if (k == "--yolo-swap-rb") nextBool(a.req.yoloSwapRB);
        else if (k == "--yolo-scale") nextFloat(a.req.yoloScale);
        else if (k == "--yolo-mean-b") nextInt(a.req.yoloMeanB);
        else if (k == "--yolo-mean-g") nextInt(a.req.yoloMeanG);
        else if (k == "--yolo-mean-r") nextInt(a.req.yoloMeanR);
        else if (k == "--yolo-opencv-backend") nextInt(a.req.yoloOpenCvBackend);
        else if (k == "--yolo-opencv-target") nextInt(a.req.yoloOpenCvTarget);

        else if (k == "--arc-backend") nextStr(a.req.arcBackend);
        else if (k == "--arc-model") nextStr(a.req.arcModelPath);
        else if (k == "--arc-config") nextStr(a.req.arcConfigPath);
        else if (k == "--arc-framework") nextStr(a.req.arcFramework);
        else if (k == "--arc-output-name") nextStr(a.req.arcOutputName);
        else if (k == "--arc-input-name") nextStr(a.req.arcInputName);
        else if (k == "--arc-w") nextInt(a.req.arcInputW);
        else if (k == "--arc-h") nextInt(a.req.arcInputH);
        else if (k == "--arc-model-version") {
            int v = 1;
            nextInt(v);
            a.req.arcModelVersion = static_cast<std::uint32_t>(std::max(0, v));
        } else if (k == "--arc-preprocess-version") {
            int v = 1;
            nextInt(v);
            a.req.arcPreprocessVersion = static_cast<std::uint32_t>(std::max(0, v));
        }

        else if (k == "--gallery-dir") nextStr(a.req.galleryDir);
        else if (k == "--topk") nextSizeT(a.req.topK);
        else if (k == "--threshold") nextFloat(a.req.acceptThreshold);
        else if (k == "--threshold-version") nextStr(a.req.thresholdVersionId);
        else if (k == "--consecutive") nextInt(a.req.consecutivePassesToTrigger);

        else if (k == "--face-select") nextStr(a.req.faceSelectPolicy);

        else if (k == "--fake-detect") nextBool(a.req.fakeDetect);
        else if (k == "--fake-embedding") nextBool(a.req.fakeEmbedding);
    }

    const auto o = runFaceInferOnce(a.req);
    std::cout << o.json << std::endl;

    std::filesystem::path dir = std::filesystem::path(o.auditDir);
    Storage::ensureDirectory(dir.string());
    const std::filesystem::path outPath = dir / o.auditFilename;
    {
        std::ofstream f(outPath, std::ios::out | std::ios::trunc);
        if (f.is_open()) f << o.json << std::endl;
    }

    return o.ok ? 0 : 1;
}

static bool readAllBytes(const std::filesystem::path& p, std::vector<std::uint8_t>& bytesOut, std::string& err) {
    err.clear();
    bytesOut.clear();
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) {
        err = "open_failed";
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streampos size = f.tellg();
    if (size < 0) {
        err = "tellg_failed";
        return false;
    }
    bytesOut.resize(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    if (!bytesOut.empty()) f.read(reinterpret_cast<char*>(bytesOut.data()), static_cast<std::streamsize>(bytesOut.size()));
    if (!f) {
        err = "read_failed";
        bytesOut.clear();
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

static std::vector<std::string> listImagesInDir(const std::filesystem::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;
    for (const auto& it : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!it.is_regular_file()) continue;
        const auto p = it.path();
        std::string ext = p.extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp") out.push_back(p.string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

static double percentileNearestRank(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    if (q <= 0.0) q = 0.0;
    if (q >= 1.0) q = 1.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    const std::size_t idx = static_cast<std::size_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(std::ceil(q * static_cast<double>(n))) - 1));
    return v[std::min(idx, n - 1)];
}

static std::string joinCommandLine(int argc, char** argv) {
    std::ostringstream oss;
    for (int i = 0; i < argc; i++) {
        if (i) oss << " ";
        const std::string s = argv[i] ? argv[i] : "";
        if (s.find(' ') != std::string::npos) oss << "\"" << s << "\"";
        else oss << s;
    }
    return oss.str();
}

static std::size_t selectMainFaceIndex(const FaceDetections& faces, const std::string& policy) {
    if (faces.empty()) return 0;
    std::string p;
    p.reserve(policy.size());
    for (char c : policy) p.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    std::size_t bestIdx = 0;
    if (p == "first") return 0;
    if (p == "area" || p == "largest") {
        for (std::size_t i = 1; i < faces.size(); i++) {
            const auto& a = faces[i];
            const auto& b = faces[bestIdx];
            const float areaA = a.bbox.width * a.bbox.height;
            const float areaB = b.bbox.width * b.bbox.height;
            if (areaA > areaB) bestIdx = i;
        }
        return bestIdx;
    }
    for (std::size_t i = 1; i < faces.size(); i++) {
        const auto& a = faces[i];
        const auto& b = faces[bestIdx];
        const float areaA = a.bbox.width * a.bbox.height;
        const float areaB = b.bbox.width * b.bbox.height;
        if (a.score > b.score) bestIdx = i;
        else if (a.score == b.score && areaA > areaB) bestIdx = i;
    }
    return bestIdx;
}

struct FaceBaselineRow {
    std::string input;
    int runIndex = 0;
    bool ok = false;
    std::string stage;
    std::string message;
    bool hasFace = false;
    std::size_t faces = 0;
    long long msLoad = 0;
    long long msDetect = 0;
    long long msAlign = 0;
    long long msEmbed = 0;
    long long msSearch = 0;
    long long msTotal = 0;
};

struct FaceBaselineStats {
    std::size_t n = 0;
    double p50 = 0.0;
    double p95 = 0.0;
};

static FaceBaselineStats computeStats(const std::vector<double>& values) {
    FaceBaselineStats s;
    s.n = values.size();
    s.p50 = percentileNearestRank(values, 0.50);
    s.p95 = percentileNearestRank(values, 0.95);
    return s;
}

static bool writeFaceBaselineRawCsv(const std::filesystem::path& path, long long tsMs, const std::vector<FaceBaselineRow>& rows) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;
    out << "ts_ms,input,run_index,ok,stage,message,has_face,faces,ms_load,ms_detect,ms_align,ms_embed,ms_search,ms_total\n";
    for (const auto& r : rows) {
        out << tsMs << ","
            << r.input << ","
            << r.runIndex << ","
            << (r.ok ? 1 : 0) << ","
            << r.stage << ","
            << r.message << ","
            << (r.hasFace ? 1 : 0) << ","
            << r.faces << ","
            << r.msLoad << ","
            << r.msDetect << ","
            << r.msAlign << ","
            << r.msEmbed << ","
            << r.msSearch << ","
            << r.msTotal
            << "\n";
    }
    return true;
}

static bool writeFaceBaselineSummaryCsv(const std::filesystem::path& path,
                                       long long tsMs,
                                       const std::string& inputPath,
                                       int warmup,
                                       int repeat,
                                       int detectStride,
                                       bool includeLoad,
                                       const FaceInferRequest& req,
                                       const FaceBaselineStats& total,
                                       const FaceBaselineStats& det,
                                       const FaceBaselineStats& emb,
                                       const FaceBaselineStats& sea,
                                       std::size_t okRuns,
                                       std::size_t errRuns) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;
    out << "ts_ms,input_path,warmup,repeat,detect_stride,include_load,yolo_backend,arc_backend,yolo_w,yolo_h,arc_w,arc_h,topk,face_select_policy,ok_runs,err_runs,"
           "n_total,p50_total_ms,p95_total_ms,n_detect,p50_detect_ms,p95_detect_ms,n_embed,p50_embed_ms,p95_embed_ms,n_search,p50_search_ms,p95_search_ms\n";
    out << tsMs << ","
        << inputPath << ","
        << warmup << ","
        << repeat << ","
        << detectStride << ","
        << (includeLoad ? 1 : 0) << ","
        << req.yoloBackend << ","
        << req.arcBackend << ","
        << req.yoloInputW << ","
        << req.yoloInputH << ","
        << req.arcInputW << ","
        << req.arcInputH << ","
        << req.topK << ","
        << req.faceSelectPolicy << ","
        << okRuns << ","
        << errRuns << ","
        << total.n << ","
        << total.p50 << ","
        << total.p95 << ","
        << det.n << ","
        << det.p50 << ","
        << det.p95 << ","
        << emb.n << ","
        << emb.p50 << ","
        << emb.p95 << ","
        << sea.n << ","
        << sea.p50 << ","
        << sea.p95
        << "\n";
    return true;
}

static bool writeFaceBaselineMarkdown(const std::filesystem::path& path,
                                     long long tsMs,
                                     const std::string& title,
                                     const std::string& cmdLine,
                                     const std::string& outDir,
                                     const std::string& stem,
                                     const FaceInferRequest& req,
                                     int warmup,
                                     int repeat,
                                     int detectStride,
                                     bool includeLoad,
                                     const FaceBaselineStats& total,
                                     const FaceBaselineStats& det,
                                     const FaceBaselineStats& emb,
                                     const FaceBaselineStats& sea,
                                     std::size_t okRuns,
                                     std::size_t errRuns) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;

    out << "# " << title << "\n\n";
    out << "- ts_ms: " << tsMs << "\n";
    out << "- 复现命令: `" << cmdLine << "`\n";
    out << "- 输出目录: `" << outDir << "`\n";
    out << "- 输出文件:\n";
    out << "  - raw_csv: `" << outDir << "/" << stem << "_raw.csv`\n";
    out << "  - summary_csv: `" << outDir << "/" << stem << "_summary.csv`\n";
    out << "  - report_md: `" << outDir << "/" << stem << ".md`\n\n";

    out << "## 参数\n\n";
    out << "- warmup: " << warmup << "\n";
    out << "- repeat: " << repeat << "\n";
    out << "- detect_stride: " << detectStride << "\n";
    out << "- include_load: " << (includeLoad ? 1 : 0) << "\n";
    out << "- yolo_backend: " << req.yoloBackend << "\n";
    out << "- yolo_input: " << req.yoloInputW << "x" << req.yoloInputH << "\n";
    out << "- yolo_score_threshold: " << req.yoloScoreThreshold << "\n";
    out << "- yolo_nms_iou_threshold: " << req.yoloNmsIouThreshold << "\n";
    out << "- yolo_kps5: " << (req.yoloEnableKeypoints5 ? 1 : 0) << "\n";
    out << "- arc_backend: " << req.arcBackend << "\n";
    out << "- arc_input: " << req.arcInputW << "x" << req.arcInputH << "\n";
    out << "- gallery_dir: " << req.galleryDir << "\n";
    out << "- topk: " << req.topK << "\n";
    out << "- face_select_policy: " << req.faceSelectPolicy << "\n\n";

    out << "## 统计口径\n\n";
    out << "- msDetect: 人脸检测（YOLO detect）耗时\n";
    out << "- msEmbed: ArcFace embedding 推理耗时\n";
    out << "- msSearch: 1:N 检索耗时\n";
    out << "- msTotal: 本次循环总耗时（包含 detect/align/embed/search；是否包含 load 由 include_load 决定；不包含模型初始化与 gallery 加载）\n\n";

    out << "## 汇总（P50/P95）\n\n";
    out << "- 统计样本: ok_runs=" << okRuns << " err_runs=" << errRuns << "\n\n";
    out << "| metric | n | p50_ms | p95_ms |\n";
    out << "|---|---:|---:|---:|\n";
    out << "| msTotal | " << total.n << " | " << total.p50 << " | " << total.p95 << " |\n";
    out << "| msDetect | " << det.n << " | " << det.p50 << " | " << det.p95 << " |\n";
    out << "| msEmbed | " << emb.n << " | " << emb.p50 << " | " << emb.p95 << " |\n";
    out << "| msSearch | " << sea.n << " | " << sea.p50 << " | " << sea.p95 << " |\n";

    return true;
}

struct FaceBaselineCliArgs {
    std::string inputPath;
    FaceInferRequest req;
    int warmup = 5;
    int repeat = 50;
    int detectStride = 1;
    bool includeLoad = false;
    std::string outDir = "tests/reports/face_baseline";
    std::string outPrefix = "face_baseline";
    std::size_t maxImages = 0;
};

static int runFaceBaselineCli(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: rk3288_cli --face-baseline <imagePath|dir> "
                     "[--warmup 5] [--repeat 50] [--detect-stride 1] [--include-load 0|1] "
                     "[--out-dir tests/reports/face_baseline] [--out-prefix face_baseline] [--max-images 0] "
                     "[--yolo-backend opencv] [--yolo-model <path>] [--yolo-config <path>] [--yolo-framework <name>] [--yolo-output-name <name>] "
                     "[--yolo-w 320] [--yolo-h 320] [--yolo-score 0.25] [--yolo-nms 0.45] [--yolo-kps5 1|0] [--yolo-letterbox 1|0] "
                     "[--yolo-swap-rb 1|0] [--yolo-scale 0.0039215686] [--yolo-mean-b 0] [--yolo-mean-g 0] [--yolo-mean-r 0] "
                     "[--yolo-opencv-backend 0] [--yolo-opencv-target 0] "
                     "[--arc-backend opencv] [--arc-model <path>] [--arc-config <path>] [--arc-framework <name>] [--arc-output-name <name>] [--arc-input-name <name>] [--arc-w 112] [--arc-h 112] "
                     "[--arc-model-version 1] [--arc-preprocess-version 1] "
                     "[--gallery-dir <dir>] [--topk N] [--face-select score_area|score|area|first] "
                     "[--fake-detect 1|0] [--fake-embedding 1|0]"
                  << std::endl;
        return 2;
    }

    FaceBaselineCliArgs a;
    a.inputPath = argv[2];

    for (int i = 3; i < argc; i++) {
        const std::string k = argv[i];
        auto nextStr = [&](std::string& v) {
            if (i + 1 < argc) v = argv[++i];
        };
        auto nextInt = [&](int& v) {
            if (i + 1 < argc) v = std::stoi(argv[++i]);
        };
        auto nextSizeT = [&](std::size_t& v) {
            if (i + 1 < argc) v = static_cast<std::size_t>(std::stoull(argv[++i]));
        };
        auto nextFloat = [&](float& v) {
            if (i + 1 < argc) v = std::stof(argv[++i]);
        };
        auto nextBool = [&](bool& v) {
            if (i + 1 < argc) v = isFlagTrue(argv[++i]);
        };

        if (k == "--warmup") nextInt(a.warmup);
        else if (k == "--repeat") nextInt(a.repeat);
        else if (k == "--detect-stride") nextInt(a.detectStride);
        else if (k == "--include-load") nextBool(a.includeLoad);
        else if (k == "--out-dir") nextStr(a.outDir);
        else if (k == "--out-prefix") nextStr(a.outPrefix);
        else if (k == "--max-images") nextSizeT(a.maxImages);

        else if (k == "--yolo-backend") nextStr(a.req.yoloBackend);
        else if (k == "--yolo-model") nextStr(a.req.yoloModelPath);
        else if (k == "--yolo-config") nextStr(a.req.yoloConfigPath);
        else if (k == "--yolo-framework") nextStr(a.req.yoloFramework);
        else if (k == "--yolo-output-name") nextStr(a.req.yoloOutputName);
        else if (k == "--yolo-w") nextInt(a.req.yoloInputW);
        else if (k == "--yolo-h") nextInt(a.req.yoloInputH);
        else if (k == "--yolo-score") nextFloat(a.req.yoloScoreThreshold);
        else if (k == "--yolo-nms") nextFloat(a.req.yoloNmsIouThreshold);
        else if (k == "--yolo-kps5") nextBool(a.req.yoloEnableKeypoints5);
        else if (k == "--yolo-letterbox") nextBool(a.req.yoloLetterbox);
        else if (k == "--yolo-swap-rb") nextBool(a.req.yoloSwapRB);
        else if (k == "--yolo-scale") nextFloat(a.req.yoloScale);
        else if (k == "--yolo-mean-b") nextInt(a.req.yoloMeanB);
        else if (k == "--yolo-mean-g") nextInt(a.req.yoloMeanG);
        else if (k == "--yolo-mean-r") nextInt(a.req.yoloMeanR);
        else if (k == "--yolo-opencv-backend") nextInt(a.req.yoloOpenCvBackend);
        else if (k == "--yolo-opencv-target") nextInt(a.req.yoloOpenCvTarget);

        else if (k == "--arc-backend") nextStr(a.req.arcBackend);
        else if (k == "--arc-model") nextStr(a.req.arcModelPath);
        else if (k == "--arc-config") nextStr(a.req.arcConfigPath);
        else if (k == "--arc-framework") nextStr(a.req.arcFramework);
        else if (k == "--arc-output-name") nextStr(a.req.arcOutputName);
        else if (k == "--arc-input-name") nextStr(a.req.arcInputName);
        else if (k == "--arc-w") nextInt(a.req.arcInputW);
        else if (k == "--arc-h") nextInt(a.req.arcInputH);
        else if (k == "--arc-model-version") {
            int v = 1;
            nextInt(v);
            a.req.arcModelVersion = static_cast<std::uint32_t>(std::max(0, v));
        } else if (k == "--arc-preprocess-version") {
            int v = 1;
            nextInt(v);
            a.req.arcPreprocessVersion = static_cast<std::uint32_t>(std::max(0, v));
        }

        else if (k == "--gallery-dir") nextStr(a.req.galleryDir);
        else if (k == "--topk") nextSizeT(a.req.topK);
        else if (k == "--face-select") nextStr(a.req.faceSelectPolicy);

        else if (k == "--fake-detect") nextBool(a.req.fakeDetect);
        else if (k == "--fake-embedding") nextBool(a.req.fakeEmbedding);
    }

    const long long tsMs = nowEpochMillis();
    const std::string stem = a.outPrefix + "_" + std::to_string(tsMs);

    Storage::ensureDirectory(a.outDir);
    const auto rawCsvPath = std::filesystem::path(a.outDir) / (stem + "_raw.csv");
    const auto summaryCsvPath = std::filesystem::path(a.outDir) / (stem + "_summary.csv");
    const auto mdPath = std::filesystem::path(a.outDir) / (stem + ".md");

    std::vector<std::string> inputs;
    std::error_code ec;
    const std::filesystem::path ip(a.inputPath);
    if (std::filesystem::is_directory(ip, ec)) {
        inputs = listImagesInDir(ip);
    } else {
        inputs.push_back(a.inputPath);
    }
    if (a.maxImages > 0 && inputs.size() > a.maxImages) inputs.resize(a.maxImages);
    if (inputs.empty()) {
        std::cerr << "FACE_BASELINE_ERROR input_empty path=" << a.inputPath << std::endl;
        return 3;
    }

    std::vector<FaceSearchEntry> galleryEntries;
    std::vector<std::string> galleryWarnings;
    std::string galleryErr;
    if (!loadGalleryDir(a.req.galleryDir, galleryEntries, galleryWarnings, galleryErr)) {
        std::cerr << "FACE_BASELINE_ERROR gallery_load_failed err=" << galleryErr << std::endl;
        return 4;
    }

    FaceSearchLinearIndex index;
    std::string indexErr;
    if (!index.reset(std::move(galleryEntries), static_cast<std::size_t>(ArcFaceEmbedding::kDim), indexErr)) {
        std::cerr << "FACE_BASELINE_ERROR index_reset_failed err=" << indexErr << std::endl;
        return 5;
    }

    std::unique_ptr<YoloFaceDetector> det;
    std::string detInitErr;
    YoloFaceOptions detOpt;
    detOpt.inputW = a.req.yoloInputW;
    detOpt.inputH = a.req.yoloInputH;
    detOpt.scoreThreshold = a.req.yoloScoreThreshold;
    detOpt.nmsIouThreshold = a.req.yoloNmsIouThreshold;
    detOpt.enableKeypoints5 = a.req.yoloEnableKeypoints5;
    detOpt.letterbox = a.req.yoloLetterbox;
    detOpt.swapRB = a.req.yoloSwapRB;
    detOpt.scale = a.req.yoloScale;
    detOpt.meanB = a.req.yoloMeanB;
    detOpt.meanG = a.req.yoloMeanG;
    detOpt.meanR = a.req.yoloMeanR;
    detOpt.opencvBackend = a.req.yoloOpenCvBackend;
    detOpt.opencvTarget = a.req.yoloOpenCvTarget;

    if (!a.req.fakeDetect) {
        if (a.req.yoloBackend == "opencv" || a.req.yoloBackend == "opencv_dnn") {
            det = CreateOpenCvDnnYoloFaceDetector();
            YoloFaceModelSpec spec;
            spec.modelPath = a.req.yoloModelPath;
            spec.configPath = a.req.yoloConfigPath;
            spec.framework = a.req.yoloFramework;
            spec.outputName = a.req.yoloOutputName;
            if (!det->load(spec, detOpt, detInitErr)) {
                std::cerr << "FACE_BASELINE_ERROR yolo_load_failed err=" << detInitErr << std::endl;
                return 6;
            }
        } else if (a.req.yoloBackend == "ncnn") {
            std::cerr << "FACE_BASELINE_ERROR yolo_load_failed err=ncnn_backend_requires_param_bin_via_yolo_face_cli" << std::endl;
            return 6;
        } else {
            std::cerr << "FACE_BASELINE_ERROR yolo_load_failed err=yolo_backend_unsupported" << std::endl;
            return 6;
        }
    }

    ArcFaceEmbedder emb;
    bool embOk = false;
    std::string embInitErr;
    if (!a.req.fakeEmbedding) {
        ArcFaceEmbedderConfig cfg;
        cfg.inputW = a.req.arcInputW;
        cfg.inputH = a.req.arcInputH;
        cfg.modelVersion = a.req.arcModelVersion;
        cfg.preprocessVersion = a.req.arcPreprocessVersion;
        cfg.opencvModel = a.req.arcModelPath;
        cfg.opencvConfig = a.req.arcConfigPath;
        cfg.opencvFramework = a.req.arcFramework;
        cfg.opencvOutput = a.req.arcOutputName;
        cfg.opencvInput = a.req.arcInputName;
        if (a.req.arcBackend == "opencv" || a.req.arcBackend == "opencv_dnn") cfg.backend = ArcFaceEmbedderConfig::BackendType::OpenCvDnn;
        else if (a.req.arcBackend == "ncnn") cfg.backend = ArcFaceEmbedderConfig::BackendType::Ncnn;
        else {
            std::cerr << "FACE_BASELINE_ERROR arc_init_failed err=arc_backend_unsupported" << std::endl;
            return 7;
        }

        embOk = emb.initialize(cfg, &embInitErr);
        if (!embOk) {
            std::cerr << "FACE_BASELINE_ERROR arc_init_failed err=" << embInitErr << std::endl;
            return 7;
        }
    }

    const int warmup = std::max(0, a.warmup);
    const int repeat = std::max(1, a.repeat);
    const int detectStride = std::max(1, a.detectStride);

    std::vector<FaceBaselineRow> rows;
    rows.reserve(static_cast<std::size_t>(inputs.size() * repeat));

    using clock = std::chrono::steady_clock;

    for (const auto& input : inputs) {
        cv::Mat baseImg;
        if (!a.includeLoad) {
            baseImg = cv::imread(input, cv::IMREAD_COLOR);
            if (baseImg.empty()) {
                std::cerr << "FACE_BASELINE_ERROR image_load_failed path=" << input << std::endl;
                return 8;
            }
        }

        FaceDetections cachedFaces;
        bool hasCachedFaces = false;

        const int totalLoops = warmup + repeat;
        for (int i = 0; i < totalLoops; i++) {
            FaceBaselineRow r;
            r.input = input;
            r.runIndex = i - warmup;
            r.ok = true;
            r.stage = "done";
            r.message = "";

            const auto t0 = clock::now();

            cv::Mat img;
            if (a.includeLoad) {
                const auto tl0 = clock::now();
                img = cv::imread(input, cv::IMREAD_COLOR);
                const auto tl1 = clock::now();
                r.msLoad = std::chrono::duration_cast<std::chrono::milliseconds>(tl1 - tl0).count();
                if (img.empty()) {
                    r.ok = false;
                    r.stage = "image_load";
                    r.message = "image_load_failed";
                }
            } else {
                img = baseImg;
            }

            FaceDetections faces;
            if (r.ok) {
                const bool needDetect = (!hasCachedFaces) || ((i % detectStride) == 0);
                if (a.req.fakeDetect) {
                    FaceDetection d;
                    d.bbox = cv::Rect2f(img.cols * 0.275f, img.rows * 0.275f, img.cols * 0.45f, img.rows * 0.45f);
                    d.score = 1.0f;
                    faces = {d};
                    hasCachedFaces = true;
                    cachedFaces = faces;
                } else if (needDetect) {
                    std::string detErr2;
                    const auto td0 = clock::now();
                    faces = det->detect(img, detErr2);
                    const auto td1 = clock::now();
                    r.msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count();
                    if (!detErr2.empty()) {
                        r.ok = false;
                        r.stage = "yolo_detect";
                        r.message = detErr2;
                    } else {
                        hasCachedFaces = true;
                        cachedFaces = faces;
                    }
                } else {
                    faces = cachedFaces;
                }
            }

            r.faces = faces.size();

            FaceDetection mainFace;
            if (r.ok && !faces.empty()) {
                r.hasFace = true;
                mainFace = faces[selectMainFaceIndex(faces, a.req.faceSelectPolicy)];
            }

            cv::Mat aligned;
            if (r.ok && r.hasFace) {
                FaceAlignOptions aopt;
                aopt.outW = a.req.arcInputW;
                aopt.outH = a.req.arcInputH;
                aopt.preferKeypoints5 = true;
                const auto ta0 = clock::now();
                const auto ar = alignFaceForArcFace112(img, mainFace, aopt);
                const auto ta1 = clock::now();
                r.msAlign = std::chrono::duration_cast<std::chrono::milliseconds>(ta1 - ta0).count();
                if (ar.alignedBgr.empty()) {
                    r.ok = false;
                    r.stage = "align";
                    r.message = ar.err.empty() ? "align_failed" : ar.err;
                } else {
                    aligned = ar.alignedBgr;
                }
            }

            std::vector<float> embedding;
            bool embeddingOk = false;
            if (r.ok && r.hasFace) {
                const auto te0 = clock::now();
                if (a.req.fakeEmbedding) {
                    embedding.assign(static_cast<std::size_t>(ArcFaceEmbedding::kDim), 0.0f);
                    embeddingOk = true;
                } else if (embOk) {
                    std::string embedErr;
                    auto e = emb.embedAlignedFaceBgr(aligned, &embedErr);
                    if (!e.has_value()) {
                        r.ok = false;
                        r.stage = "arc_embed";
                        r.message = embedErr.empty() ? "arc_embed_failed" : embedErr;
                    } else {
                        embedding = std::move(e->values);
                        embeddingOk = (embedding.size() == static_cast<std::size_t>(ArcFaceEmbedding::kDim));
                        if (!embeddingOk) {
                            r.ok = false;
                            r.stage = "arc_embed";
                            r.message = "embed_dim_mismatch";
                        }
                    }
                } else {
                    r.ok = false;
                    r.stage = "arc_init";
                    r.message = "arc_not_initialized";
                }
                const auto te1 = clock::now();
                r.msEmbed = std::chrono::duration_cast<std::chrono::milliseconds>(te1 - te0).count();
            }

            if (r.ok && r.hasFace && embeddingOk) {
                const auto ts0 = clock::now();
                std::string searchErr;
                if (index.size() > 0) {
                    FaceSearchOptions opt;
                    opt.assumeL2Normalized = true;
                    (void)index.searchTopK(embedding, a.req.topK, opt, searchErr);
                }
                const auto ts1 = clock::now();
                r.msSearch = std::chrono::duration_cast<std::chrono::milliseconds>(ts1 - ts0).count();
                if (!searchErr.empty()) {
                    r.ok = false;
                    r.stage = "search";
                    r.message = searchErr;
                }
            }

            const auto t1 = clock::now();
            r.msTotal = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

            if (i >= warmup) rows.push_back(std::move(r));
        }
    }

    std::size_t okRuns = 0;
    std::size_t errRuns = 0;
    std::vector<double> totalMs;
    std::vector<double> detMs;
    std::vector<double> embMs;
    std::vector<double> seaMs;
    totalMs.reserve(rows.size());
    detMs.reserve(rows.size());
    embMs.reserve(rows.size());
    seaMs.reserve(rows.size());

    for (const auto& r : rows) {
        if (r.ok) okRuns++;
        else errRuns++;
        if (!r.ok || !r.hasFace) continue;
        totalMs.push_back(static_cast<double>(r.msTotal));
        detMs.push_back(static_cast<double>(r.msDetect));
        embMs.push_back(static_cast<double>(r.msEmbed));
        seaMs.push_back(static_cast<double>(r.msSearch));
    }

    const auto total = computeStats(totalMs);
    const auto detS = computeStats(detMs);
    const auto embS = computeStats(embMs);
    const auto seaS = computeStats(seaMs);

    bool ok = true;
    ok = ok && writeFaceBaselineRawCsv(rawCsvPath, tsMs, rows);
    ok = ok && writeFaceBaselineSummaryCsv(summaryCsvPath, tsMs, a.inputPath, warmup, repeat, detectStride, a.includeLoad, a.req, total, detS, embS, seaS, okRuns, errRuns);
    ok = ok && writeFaceBaselineMarkdown(mdPath, tsMs, "RK3288 人脸推理性能基线报告", joinCommandLine(argc, argv), a.outDir, stem, a.req, warmup, repeat, detectStride, a.includeLoad, total, detS, embS, seaS, okRuns, errRuns);
    if (!ok) {
        std::cerr << "FACE_BASELINE_ERROR write_failed out_dir=" << a.outDir << std::endl;
        return 9;
    }

    std::cout << "FACE_BASELINE_RESULT"
              << " p50_total_ms=" << total.p50
              << " p95_total_ms=" << total.p95
              << " p50_detect_ms=" << detS.p50
              << " p95_detect_ms=" << detS.p95
              << " p50_embed_ms=" << embS.p50
              << " p95_embed_ms=" << embS.p95
              << " p50_search_ms=" << seaS.p50
              << " p95_search_ms=" << seaS.p95
              << " ok_runs=" << okRuns
              << " err_runs=" << errRuns
              << std::endl;

    std::cout << "FACE_BASELINE_OUTPUT"
              << " out_dir=" << a.outDir
              << " stem=" << stem
              << std::endl;

    return 0;
}

int main(int argc, char** argv) {
    std::cout << "Starting RK3288 AI Engine (CLI Mode)..." << std::endl;

    if (argc > 1 && std::string(argv[1]) == "--analyze") {
        if (argc < 4) {
            std::cerr << "Usage: rk3288_cli --analyze <imagePath> <cascadePath> [outDir]" << std::endl;
            return 2;
        }
        std::string outDir = "";
        if (argc > 4) outDir = argv[4];
        return runAnalyze(argv[2], argv[3], outDir);
    }

    if (argc > 1 && std::string(argv[1]) == "--yolo-face") {
        return runYoloFaceDetect(argc, argv);
    }

    if (argc > 1 && std::string(argv[1]) == "--face-infer") {
        return runFaceInferCli(argc, argv);
    }

    if (argc > 1 && std::string(argv[1]) == "--face-baseline") {
        return runFaceBaselineCli(argc, argv);
    }

    // Parse command line arguments
    int cameraId = 0;
    std::string inputPath = "";
    bool useFile = false;
    std::string cascadePath = "tests/data/lbpcascade_frontalface.xml"; // Default for host test
    std::string storagePath = "host_storage";

#ifdef __ANDROID__
    cascadePath = "/data/local/tmp/lbpcascade_frontalface.xml";
    storagePath = "/data/local/tmp/rk3288_data";
#endif

    if (argc > 1) {
        try {
            size_t idx;
            cameraId = std::stoi(argv[1], &idx);
            if (idx < std::string(argv[1]).length()) {
                // Not a pure number, treat as file path
                throw std::invalid_argument("Not a number");
            }
            std::cout << "Using Camera ID: " << cameraId << std::endl;
        } catch (...) {
            // Treat as file path
            inputPath = argv[1];
            useFile = true;
            std::cout << "Using Input File: " << inputPath << std::endl;
        }
    }

    if (argc > 2) {
        cascadePath = argv[2];
    }

    if (argc > 3) {
        storagePath = argv[3];
    }

    int maxFrames = 0;
    if (argc > 4) {
        try {
            maxFrames = std::stoi(argv[4]);
        } catch (...) {}
    }

    std::cout << "Cascade Path: " << cascadePath << std::endl;
    std::cout << "Storage Path: " << storagePath << std::endl;
    if (maxFrames > 0) std::cout << "Max Frames: " << maxFrames << std::endl;

    // Register signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    Engine engine;
    if (maxFrames > 0) engine.setMaxFrames(maxFrames);
    g_engine.store(&engine);

    bool initResult = false;
    if (useFile) {
        initResult = engine.initialize(inputPath, cascadePath, storagePath);
    } else {
        initResult = engine.initialize(cameraId, cascadePath, storagePath);
    }

    if (!initResult) {
        std::cerr << "Engine initialization failed." << std::endl;
        return 1;
    }

    // Run the main loop (blocking)
    engine.run();

    std::cout << "Engine stopped. Exiting." << std::endl;
    return 0;
}
