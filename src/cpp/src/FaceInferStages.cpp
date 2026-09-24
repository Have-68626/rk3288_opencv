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

    // Fallback: 创建临时检测器（无缓存，兼容旧调用方）
    //
    // ⚠️ 可达性（原注释「INT8 量化模型优先选择」失真，已订正）：这不是「INT8 优先」策略，
    //    而是**上游未注入实例时的回落排序**。本段位于上面的 `if (ctx.detector)` 守卫**之后**，
    //    只有注入失败（runFaceInferOnce 在 FaceInferencePipeline.cpp 里对 ctx.detector 是
    //    **无条件赋值** `localDet.get()`，初始化失败时赋的正是 nullptr）才会执行；注入成功的
    //    正常路径已在守卫处直接返回，**根本到不了这里**。段内顺序为 INT8 先、FP32 后。
    // ⚠️ 其中 `req.int8Enabled == true` 的子分支额外要求调用方显式开启 INT8，且注册表里存在
    //    可 load 的 INT8 检测器。生产者现已落地（原「全仓无任何调用点置真」已不成立）：
    //      · `rk3288_cli --face-infer` 的 `--int8 1|0`（src/cpp/main.cpp）——**唯一**能走到本段的
    //        CLI 生产者（`--face-baseline` 也解析该参数，但其基准路径不调用 runFaceInferOnce
    //        ⇒ 到不了本段，详见 main.cpp 中 `--face-baseline` 的解析处注释）；
    //      · 测试 `tests/cpp/test_face_infer_pipeline.cpp` 的 int8 用例
    //        （✅ 已核实：该用例已在 `tests/cpp/face_infer_unit_tests_main.cpp` 的 TestCase 表里注册，
    //        会随 `face_infer_unit_tests` 执行；注册表若被改动删掉该条目，这条生产者即重新归零）。
    //    ⚠️ 但 `WinConfig.model.int8Enabled` **仍不是**该字段的生产者：Windows 运行时（FramePipeline /
    //      RuntimeBootstrap）根本不构造 `FaceInferRequest`，该键在 Windows 侧只影响 RuntimeBootstrap
    //      启动期的快照与告警（见 RuntimeBootstrap.cpp 的 INT8 消费段）。
    //    注册表侧另有前置条件：`models/yolo_face_int8_ncnn/yolo_face_int8.param` 存在时才会注册
    //    `yolo_face_int8`（ModelRegistry::ensureBuiltinRegistered），故该子分支是否真能命中还取决于模型文件。
    // ⚠️ **本支未做节点名注入**（如实登记，与下方 computeEmbedding 的 `arcface_int8` 支不对称）：
    //    `YoloFaceAdapter`（adapters/YoloFaceAdapter.h）**没有** `setNodeNames` 之类入口，
    //    其节点名只能由实现内部的默认值决定，调用点无从覆盖 ⇒ 若 INT8 检测模型的节点名不是该默认值，
    //    本支同样会按返回码失败并被跳过（不会抛出、不会影响返回值）。修它需要改适配器接口（不在本次范围）。
    // ⚠️ 本段**不得删除**：删掉它会让「注入失败 + int8Enabled」这一配置失去可能恢复的路径，
    //    直接变成硬失败。
    std::string detErr;

    if (req.int8Enabled) {
        std::string loadErr;
        ModelRegistry::ensureBuiltinRegistered();
        auto int8Det = ModelRegistry::instance().createDetector("yolo_face_int8");
        if (int8Det && int8Det->load(req.yoloModelPath, loadErr)) {
            ctx.yoloBackendName = std::string(int8Det->name()) + " (INT8)";
            const auto td0 = std::chrono::steady_clock::now();
            ctx.faces = int8Det->detect(ctx.img, detErr);
            const auto td1 = std::chrono::steady_clock::now();
            m.msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count();
            if (detErr.empty()) return okStatus();
        }
        // 与下面 computeEmbedding 的 INT8 回退同口径：把两段 err 文本都带出来。此前只带
        // loadErr，一旦是「load 成功但 detect 失败」就会打出 "failed: , falling back"，
        // detErr（真正的失败原因）被静默丢弃。
        // 安全边界：err 文本只含节点名 / 维度 / 路径，不含模板数据；且只落本地日志，
        // 不参与 reason、不回传调用方。
        rklog::logWarn("FaceInferStages", "detectFaces",
                       "INT8 detector failed: load=" + loadErr + " detect=" + detErr +
                           ", falling back to FP32");
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
        std::unique_ptr<YoloFaceDetector> det = CreateOpenCvDnnYoloFaceDetector();
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

        ctx.yoloBackendName = det->backendName();
        const auto td0 = std::chrono::steady_clock::now();
        ctx.faces = det->detect(ctx.img, detErr);
        const auto td1 = std::chrono::steady_clock::now();
        m.msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count();
        if (!detErr.empty()) {
            return failStatus("yolo_detect", detErr);
        }
        return okStatus();
    } else {
        return failStatus("yolo_load", "yolo_backend_unsupported");
    }
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
        return okStatus();
    }

    // 使用外部注入的嵌入器实例（调用方管理生命周期，每帧复用避免重复加载模型）
    if (ctx.embedder) {
        std::string embedErr;
        auto e = ctx.embedder->embedAlignedFaceBgr(ctx.aligned112, &embedErr);
        if (e.has_value()) {
            ctx.embedding = std::move(e->values);
            ctx.embeddingOk = true;
            ctx.arcBackendName = req.arcBackend;
            const auto te1 = std::chrono::steady_clock::now();
            m.msEmbed = std::chrono::duration_cast<std::chrono::milliseconds>(te1 - te0).count();
            return okStatus();
        }
        return failStatus("arc_embed", embedErr.empty() ? "arc_embed_failed" : embedErr);
    }

    // Fallback: 创建临时嵌入器（无缓存，兼容旧调用方）
    //
    // ⚠️ 可达性（原注释「INT8 量化模型优先选择」失真，已订正）：这不是「INT8 优先」策略，
    //    而是**上游未注入实例时的回落排序**。本段位于上面的 `if (ctx.embedder)` 守卫**之后**，
    //    只有注入失败（runFaceInferOnce 在 FaceInferencePipeline.cpp 里对 ctx.embedder 是
    //    **无条件赋值** `localEmb.get()`，初始化失败时赋的正是 nullptr）才会执行；注入成功的
    //    正常路径已在守卫处直接返回，**根本到不了这里**。段内顺序为 INT8 先、FP32 后。
    // ⚠️ 其中 `req.int8Enabled == true` 的子分支额外要求调用方显式开启 INT8，且注册表里存在
    //    可 load 的 INT8 嵌入器。生产者现已落地（原「全仓无任何调用点置真」已不成立）：
    //      · `rk3288_cli --face-infer` 的 `--int8 1|0`（src/cpp/main.cpp）——**唯一**能走到本段的
    //        CLI 生产者（`--face-baseline` 也解析该参数，但其基准路径不调用 runFaceInferOnce
    //        ⇒ 到不了本段，详见 main.cpp 中 `--face-baseline` 的解析处注释）；
    //      · 测试 `tests/cpp/test_face_infer_pipeline.cpp` 的 int8 用例
    //        （✅ 已核实：该用例已在 `tests/cpp/face_infer_unit_tests_main.cpp` 的 TestCase 表里注册，
    //        会随 `face_infer_unit_tests` 执行；注册表若被改动删掉该条目，这条生产者即重新归零）。
    //    ⚠️ 但 `WinConfig.model.int8Enabled` **仍不是**该字段的生产者：Windows 运行时（FramePipeline /
    //      RuntimeBootstrap）根本不构造 `FaceInferRequest`，该键在 Windows 侧只影响 RuntimeBootstrap
    //      启动期的快照与告警（见 RuntimeBootstrap.cpp 的 INT8 消费段）——勿把两者读成同一条通路。
    //    注册表侧另有前置条件：`models/arcface_int8_ncnn/arcface_int8.param` 存在时才会注册
    //    `arcface_int8`（`models/mobilefacenet_int8_ncnn/…` 同理学 `mobilefacenet_int8`），
    //    见 ModelRegistry::ensureBuiltinRegistered。
    // ⚠️ 本段**不得删除**：删掉它会让「注入失败 + int8Enabled」这一配置失去可能恢复的路径，
    //    直接变成硬失败。
    if (req.int8Enabled) {
        ModelRegistry::ensureBuiltinRegistered();
        // 按优先级尝试 INT8 识别器。
        //
        // ⚠️ **一个模型路径喂两个 id（如实登记：这不是「按 id 选模型」）**：下表两个 id 拿到的都是
        //    同一个 `req.arcModelPath`。`FaceInferRequest`（FaceInferencePipeline.h）**没有**独立的
        //    mobilefacenet 模型路径字段，本函数也无权新增配置面 ⇒ id 实际只决定「用哪个适配器类」，
        //    **不决定用哪个模型文件**。后果（fail-open，非 fail-closed）：当 `req.arcModelPath` 指向
        //    `mobilefacenet_int8` 的 `.param` 时，首选的 `arcface_int8`（ArcFaceAdapter）会拿该路径去
        //    加载——两个 INT8 模型的节点名同为 `in0`/`out0`、输出同为 512 维 ⇒ **可能加载成功**，
        //    于是 `ctx.arcBackendName` 被**误标**为 ArcFace INT8。真正修好需新增配置面（每 id 独立
        //    模型路径），不在本函数权限内 ⇒ **本缺陷只如实加注，id 表与循环逻辑一字不改**；
        //    下一条 ⚠️ 记的行为变化来自**节点名注入**这一独立修复，二者勿混。
        //    ⚠️ **行为变化（本条修复的副作用，如实登记）**：节点名注入落地**前**，`arcface_int8` 会因
        //    默认 `data`/`output` 在 `ex.input()` 处失败 ⇒ 由 `mobilefacenet_int8` 接手（标签与路径
        //    相符）；落地**后**，同一路径可能被首选支直接吃掉（标签与实际模型不符）。嵌入**取值**仍
        //    来自该 `.param`（未串模型、未串库），差异只落在 `arcBackendName` 这一诊断字段上。
        //    ⚠️ 且两条适配器的**预处理不同**（实测于源码）：`ArcFaceEmbedder` 的 ncnn 路径为
        //    mean=(127.5,127.5,127.5) / scale=1/127.5（ArcFaceEmbedderConfig 默认），而
        //    `MobileFaceNetAdapter` 为 mean=0 / scale=1/127.5（见该文件 embed()）——两者差一个 127.5 的
        //    均值偏移 ⇒ 首选支吃掉 mobilefacenet 路径时，**嵌入取值本身也会不同**（不只是标签）。
        //    该数值差异**未被任何测试覆盖**，按 mobilefacenet 路径部署前必须实测。
        // 注入闸门：只在**本次 load 会选到 ncnn 后端**时才注入节点名。判据复刻
        // `ArcFaceAdapter::load` 的后端选择规则（扩展名 `.param` / `.bin` → ncnn，其余 → OpenCvDnn，
        // 见 src/cpp/src/adapters/ArcFaceAdapter.cpp）；`load()` 用的正是同一 `req.arcModelPath`。
        // ⚠️ 为什么必须有这道闸门（本条注入修复的副作用，如实登记）：
        //    `ArcFaceAdapter::setNodeNames` 会**同时**写 `ncnnInput/ncnnOutput` **和**
        //    `opencvInput/opencvOutput`；而 `ArcFaceEmbedderConfig` 的 opencv 侧默认是
        //    **空串 = 自动推断**（ArcFaceEmbedder.h；消费点见 ArcFaceEmbedder.cpp 的
        //    `if (!cfg_.opencvInput.empty())` / `if (cfg_.opencvOutput.empty())`）。
        //    ⇒ 若不加闸门：`--int8 1` 且 `arcModelPath` 是 `.onnx`（后端 OpenCvDnn）时，本注入会把
        //    「自动推断」变成强制 `"in0"`/`"out0"` ⇒ `net.setInput(blob, "in0")` 抛 `cv::Exception`，
        //    被 ArcFaceEmbedder.cpp 的 catch 吞成 err 文本 ⇒ **该支静默失败**（不崩溃：`embed()`
        //    返回 nullopt ⇒ 循环落空 ⇒ 由 `mobilefacenet_int8`、最终 FP32 接手，仅下面那条 warn 可见）。
        //    即「一处为修 ncnn 而做的注入把 OpenCV DNN 侧一并改坏」。
        //    ⚠️ 本闸门**不改** `ArcFaceAdapter`（不在本批改动面）：其「同步写两组节点名」的语义仍在，
        //    只是该副作用不再被本调用点触发。换用非 `.param`/`.bin` 扩展名的 ncnn 模型时本注入会失效。
        //    ⚠️ 连带影响（**本批已同批处理完毕**，勿再按「待办」读）：`tests/cpp/test_face_infer_pipeline.cpp`
        //    的 `test_face_infer_int8_chain_consumes_flag_and_injects_arcface_node_names` 原先用
        //    `int8_chain_on.onnx` 作「开」侧请求路径并断言节点名**已被注入** in0/out0——本闸门下该断言
        //    不再成立 ⇒ 该用例的「开」侧路径**已改为** `int8_chain_on.param`（「关」侧仍用
        //    `int8_chain_off.onnx`）：既踩到闸门的 ncnn 分支、又保住注入断言，且与 `--int8` 的文档
        //    前置条件（ncnn `.param`/`.bin`）语义一致。**该文件就在本次改动面内**（已随之修改）。
        const std::string arcModelExt = std::filesystem::path(req.arcModelPath).extension().string();
        const bool arcModelIsNcnn = (arcModelExt == ".param" || arcModelExt == ".bin");

        const char* int8EmbedderIds[] = {"arcface_int8", "mobilefacenet_int8"};
        for (const char* eId : int8EmbedderIds) {
            auto int8Emb = ModelRegistry::instance().createEmbedder(eId);
            if (int8Emb) {
                // ★ 两分支默认值不对称的修复：给 `arcface_int8` 分支**显式注入**真实节点名。
                //   缘由：`ArcFaceAdapter` / `ArcFaceEmbedderConfig` 的节点名默认是 `data`/`output`
                //   （ncnn 字段默认值），而本仓两个 INT8 ncnn 模型的 `.param` 首层为 `Input in0`、
                //   末层为 `… out0` ⇒ 用默认名走 ncnn 必然在 `ex.input()` 处失败；ncnn 失败是
                //   **返回码而非异常**，循环**不会中断**，只会跳到下一个 id（与 FP32 分支的
                //   `failStatus("arc_init")` 不同：这条路径不改变出参、不影响返回值，只多一行 warn
                //   ——每个失败支各一行，故最坏 2 行）
                //   ⇒ 首选分支永不生效、优先级被反转，实际改由 `mobilefacenet_int8` 承担。
                //   对照：`mobilefacenet_int8` 走
                //   `MobileFaceNetAdapter`，其默认已是 `in0`/`out0`（MobileFaceNetAdapter.h），本就正确
                //   ⇒ 只有 ArcFace 这一支需要补。
                //   ⚠️ **不**改 `ArcFaceEmbedderConfig` / `ArcFaceAdapter` 的全局默认：`data`/`output`
                //   是既有契约，`tests/cpp/test_arcface_contract.cpp` 的 NodeNamesConfigDriven /
                //   AdapterNodeNamesInjectedFromManifest / AdapterNodeNameEmptyIgnored 三用例**主动
                //   要求**该默认值，改默认会让那三条变红；本处按「具体调用点注入」而非「改默认」。
                //   ⚠️ 语义提醒：`ArcFaceAdapter::setNodeNames` 是**空串忽略**（保留既有值），与
                //   `ArcFaceWinRecognizer::setNodeNames` 的**空串 = 显式置空**不同。此处传的都是非空
                //   字面量，不落在「空串忽略」分支上；且必须在 `load()` 前调用（load 只按扩展名选后端）。
                //   ⚠️ 已知取舍：本支**不读** `req.arcNcnnInput/arcNcnnOutput`（其默认值是错的
                //   `data`/`output`，且无法区分「用户显式传 data」与「沿用默认」），一律按本仓 INT8
                //   模型的真实节点名 `in0`/`out0` 注入；换用节点名不同的 INT8 模型时需另加配置面。
                //   ⚠️ 且只在 `arcModelIsNcnn` 为真（后端是 ncnn）时注入，理由见循环上方闸门注释：
                //   `setNodeNames` 会连带改写 opencv 侧节点名，在 OpenCvDnn 后端上是净损害。
                if (auto* arcAdapter = dynamic_cast<ArcFaceAdapter*>(int8Emb.get())) {
                    if (arcModelIsNcnn) {
                        arcAdapter->setNodeNames("in0", "out0");
                    }
                    // 走上非 ncnn 后端时不注入：此时 `opencvInput` 保持空串=自动推断，刻意不动。
                } else if (std::string(eId) == "arcface_int8") {
                    // ⚠️ 静默分支（本批登记并补一条**一次性**日志）：期望 `arcface_int8` 的工厂给出
                    //    `ArcFaceAdapter`，否则**既不注入、也不报错**，随后只会出现下面那条
                    //    「load/embed 失败」的 warn —— 排障者会顺着它去查**模型文件 / 节点名**，
                    //    而真实原因是**注册表里该 id 注册的动态类型变了**（custom 适配器 / 注册顺序）。
                    //    排查方向：先看 `ModelRegistry` 中 `arcface_int8` 的注册类型，再查模型。
                    //    频率：本函数是逐帧路径 ⇒ **只在首次**记一次（static + exchange 原子，
                    //    多线程下最多一条），不叠加逐帧刷屏。`mobilefacenet_int8` 本就应是别的类型
                    //    （`MobileFaceNetAdapter`），故只对 `arcface_int8` 报，避免误报噪声。
                    static std::atomic<bool> s_arcfaceInt8TypeMissLogged{false};
                    if (!s_arcfaceInt8TypeMissLogged.exchange(true)) {
                        rklog::logWarn("FaceInferStages", "computeEmbedding",
                                       "INT8 embedder(arcface_int8) 的动态类型不是 ArcFaceAdapter"
                                       " ⇒ 无法注入节点名 in0/out0，本支将按默认 data/output 走并失败；"
                                       "请先查 ModelRegistry 中该 id 的注册类型，而非模型文件");
                    }
                }
                std::string loadErr;
                // embedErr 特意提到本层（而非 load 分支内）：下面的 warn 需要把 load / embed
                // 两段 err 文本一并带出，详见该 warn 处的说明。
                std::string embedErr;
                if (int8Emb->load(req.arcModelPath, loadErr)) {
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
                // 清单条目 S2：MobileFaceNetAdapter 的 embed() 对「输入节点不存在 / 输出节点提取
                // 失败 / 输出维度异常 / 输出维度不符」写的是**四类差异化文本**，此前在消费点被
                // 换成了通用文案，差异化诊断到不了日志。此处把 eId 与 load/embed 两段 err 一并
                // 带出（此前 embedErr 完全不可见）。
                // 安全边界：err 文本只含模型节点名 / 维度 / 文件路径，**不含任何模板数据**；
                // 且**只落本地日志**——不影响 reason、不影响返回值、不外传给调用方。
                // 频率：最坏每帧 2 行（上面的 id 数组两个元素），且仅在 req.int8Enabled 时执行，
                // 可接受 ⇒ **无需限频**，后人勿在此叠加节流逻辑。
                // （例外：类型不符那条 warn 不在此上界内——它是**一次性**的，见循环内注释。）
                rklog::logWarn("FaceInferStages", "computeEmbedding",
                               "INT8 embedder(" + std::string(eId) + ") failed: load=" + loadErr +
                                   " embed=" + embedErr + ", falling back to FP32");
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

    {
        auto e = emb.embedAlignedFaceBgr(ctx.aligned112, &initErr);
        if (e.has_value()) {
            ctx.embedding = std::move(e->values);
            ctx.embeddingOk = true;
        }
    }

    if (!ctx.embeddingOk) {
        return failStatus("arc_embed", "arc_embed_failed");
    }
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

