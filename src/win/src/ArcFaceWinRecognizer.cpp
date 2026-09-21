#include "rk_win/ArcFaceWinRecognizer.h"
#include "rk_win/FaceRecognizer.h"

using namespace rk_core;

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace rk_win {

bool ArcFaceWinRecognizer::initialize(const std::string& cascadePath, const std::filesystem::path& dbPath,
                                       const std::string& arcFaceModelPath, int minFaceSizePx, double identifyThreshold) {
    if (!detector_.loadCascade(cascadePath)) return false;

    embedderCfg_.backend = ArcFaceEmbedderConfig::BackendType::OpenCvDnn;
    embedderCfg_.opencvModel = arcFaceModelPath;
    embedderCfg_.opencvFramework = "onnx";
    // ── 节点名来源（FACE-ARCWIN-001 已修复）──
    // 背景：`ArcFaceEmbedder` 的类型本身**并非没有注入能力**——其 `ArcFaceEmbedderConfig` 就带
    // `opencvInput`/`opencvOutput`/`ncnnInput`/`ncnnOutput` 字段。修复前本类**曾把节点名硬编码**、
    // 且**无外部覆盖路径**，当时是全部 ArcFace 装配路径中**唯一把节点名硬编码的生产点**（initialize
    // 形参不含节点名，WinConfig/WinJsonConfig 确无对应键，亦无环境变量入口）。
    // ⚠️ **修复后该「唯一硬编码」描述不再成立**：本类已改为「默认留空 + `setNodeNames()` 注入」，
    // 且 `WinConfig`/`WinJsonConfig` 现已新增 `arcFaceInputName`/`arcFaceOutputName` 配置键（见
    // `WinConfig.h` 的 `RecognitionConfig`）——即节点名现可经配置注入。
    // 其余三处直接实例化 `ArcFaceEmbedder` 的生产/CLI 点（`FaceInferStages.cpp`、
    // `FaceInferencePipeline.cpp`、`main.cpp`）至今仍无 setter，但都从请求字段注入节点名。注册表
    // 路径同样不注入节点名，但其 `ArcFaceAdapter` **具备**该能力（只是未被调用）（对照：Android
    // 实时链经 manifest 注入 `in0`/`out0`）。
    // ⚠️ 曾把 `opencvInput`/`opencvOutput` 硬编码为 `data`/`output`，**已被实测证实为真实
    // 缺陷**（不是「疑似」）：以**未改动**的 `ArcFaceEmbedder.cpp` 加独立探针**真编译真运行**
    // （MSVC `cl /std:c++17 /O2 /MD`，链接本仓既有 OpenCV 4.10.0 共享库），对同一模型做三组对照
    // ——(A) 生产现状 `data`/`output`：`initialize` 成功，但**推理期** OpenCV 抛
    // `error: (-204:Requested object was not found) Requested blob "data" not found in function
    // 'cv::dnn::dnn4_v20240521::Net::Impl::setInput'`，退出码 3；(B) 两字段置空 `''`/`''`：
    // **成功，512 维，退出码 0**；(C) 真实名 `input.1`/`683`：**成功，512 维，退出码 0**。B、C
    // 两组 `first3` 与 `l2norm^2=1.000001` **逐位相同**，即命中同一条有效推理路径。另探针枚举该图
    // OpenCV 视角共 **132** 层，**无任何层名为 `data`/`output`**，末层 `name=683`。交叉验证
    // `w600k_r50.pnnxsim.onnx`（`in0`/`out0` 命名系）同样 A 失败、B 成功。
    // 证据边界（如实、不夸大）：生产默认权重 `models/arcface_w600k_r50.onnx`（`WinConfig.cpp` /
    // `WinJsonConfig.cpp`）当前**不在仓、不在盘**，**未对该确切文件取证**；但**仓内全部可得的
    // ArcFace ONNX 均不用 `data`/`output` 命名**——`deps/insightface/w600k_r50.onnx` 为 `input.1` /
    // `683`，`w600k_r50.pnnxsim.onnx` 及转换脚本 `w600k_r50_pnnx.py`（`input_names=['in0']`）为
    // `in0` / `out0`，ncnn 侧 `models/arcface_ncnn/arcface.param` 亦为 `Input in0`…`out0`；且
    // `data`/`output` 恰为 `ArcFaceEmbedderConfig` 的 **ncnn 字段默认值**（`ArcFaceEmbedder.h`），
    // 即把该 ncnn 默认误当作 OpenCV-DNN 的真实节点名（探针为**独立探针 + 合成图 + `/MD`**，非本目标
    // 构建）。
    // ✅ **修法（方案 3，已实施）**：三管齐下——
    //   (1) **默认留空**：本类不再写 `opencvInput`/`opencvOutput`，沿用 `ArcFaceEmbedderConfig` 的
    //       空串默认 ⇒ OpenCV-DNN 路径走「自动默认输入 / 首个输出」（这正是修复本身）；
    //   (2) **可注入**：新增 `setNodeNames()`（须在 `initialize()` 前调用），供换权重/换命名系时覆盖；
    //   (3) **自检**：`initialize()` 收尾阶段（在 `db_.load()` **之前**）用合成图跑一次前向，失败即 fail loud（见函数体）。
    // ⚠️ 换用节点名不同的权重时，`ArcFaceEmbedder::initialize` **不校验节点名**，注入缺失或写错
    // 都不会在加载期暴露——OpenCV-DNN 路径仅在节点名为**空串**时才走「自动默认输入 / 首个输出」
    // （`ArcFaceEmbedder.cpp`）；真正的失败**推迟到推理阶段**——`setInput`/`forward` 抛异常被捕获
    // 后返回空，`identify()` 对**所有人脸恒返回 UNKNOWN**（完全静默）。正因如此才补了 (3) 自检。
    // 节点名不再在此硬编码：`embedderCfg_` 的 `opencvInput`/`opencvOutput` 默认即空串（自动），
    // 需要覆盖时经 `setNodeNames()` 注入（见上方时序契约）。
    // ⚠️ 勿把 `data`/`output` 作为回退默认值重新引入本类——它们只是 `ArcFaceEmbedderConfig` 的
    // ncnn 字段默认值，**不是** OpenCV-DNN 的真实节点名。

    std::string err;
    if (!embedder_.initialize(embedderCfg_, &err)) {
        std::cerr << "[ArcFaceWinRecognizer] initialize failed: " << err << std::endl;
        return false;
    }

    dbPath_ = dbPath;
    minFaceSizePx_ = minFaceSizePx;
    identifyThreshold_ = identifyThreshold;

    // ── 初始化自检（fail loud，FACE-ARCWIN-001 的防静默关键）──
    // 理由：嵌入器加载期**不校验节点名**（`ArcFaceEmbedder::initialize` 只 load 网络），节点名错 /
    //   模型不可用的真正失败会**推迟到推理期**——`setInput`/`forward` 抛异常被捕获后返回空，
    //   `identify()` 对所有人脸恒返回 UNKNOWN（完全静默）。若没有本自检，任何节点名/模型问题
    //   都要等到线上识别才发现。用一张合成图跑一次前向即可在启动期暴露。
    // 代价：启动多一次模型前向（量级约 10–50ms，随模型/后端浮动）。
    {
        cv::Mat selfCheckProbe(112, 112, CV_8UC3, cv::Scalar(127, 127, 127));
        std::string selfCheckErr;
        auto selfCheck = embedder_.embedAlignedFaceBgr(selfCheckProbe, &selfCheckErr);
        if (!selfCheck.has_value()) {
            std::cerr << "[ArcFaceWinRecognizer] 初始化自检失败（节点名或模型不可用）: "
                      << selfCheckErr << std::endl;
            return false;
        }
    }

    return db_.load(dbPath_);
}

void ArcFaceWinRecognizer::setNodeNames(std::string input, std::string output) {
    // 空串 = 自动：直接写入空串，由 OpenCV 取「默认输入 / 首个输出」兜底，此处**不**替换成任何默认值。
    // （注意与 `ArcFaceAdapter::setNodeNames` 的「空串忽略」语义不同——那里空串保留既有值。）
    // 时序契约：须在 initialize() 之前调用（initialize 内部才把 cfg 交给 embedder_）。
    // 忘记调用不会静默——initialize 收尾阶段（`db_` 加载**之前**）的自检会 fail loud。
    embedderCfg_.opencvInput = std::move(input);
    embedderCfg_.opencvOutput = std::move(output);
}

cv::Rect ArcFaceWinRecognizer::pickLargest(const std::vector<cv::Rect>& faces) {
    if (faces.empty()) return {};
    auto best = faces[0];
    for (const auto& r : faces) {
        if (r.area() > best.area()) best = r;
    }
    return best;
}

cv::Mat ArcFaceWinRecognizer::cropAndNormalizeFaceBgr(const cv::Mat& bgr, const cv::Rect& faceRect) {
    cv::Rect r = faceRect & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (r.empty()) return {};

    const int padX = static_cast<int>(r.width * 0.15);
    const int padY = static_cast<int>(r.height * 0.15);
    r.x = std::max(0, r.x - padX);
    r.y = std::max(0, r.y - padY);
    r.width = std::min(bgr.cols - r.x, r.width + padX * 2);
    r.height = std::min(bgr.rows - r.y, r.height + padY * 2);

    cv::Mat face;
    cv::resize(bgr(r), face, cv::Size(112, 112), 0, 0, cv::INTER_LINEAR);
    return face;
}

void ArcFaceWinRecognizer::l2NormalizeInplace(std::vector<float>& v) {
    float sqSum = 0.0f;
    for (float x : v) sqSum += x * x;
    if (sqSum < 1e-12f) return;
    float inv = 1.0f / std::sqrt(sqSum);
    for (float& x : v) x *= inv;
}

float ArcFaceWinRecognizer::cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 0.0f;
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float denom = std::sqrt(na) * std::sqrt(nb);
    return (denom < 1e-12f) ? 0.0f : dot / denom;
}

std::vector<FaceMatch> ArcFaceWinRecognizer::identify(const cv::Mat& bgr) {
    std::vector<FaceMatch> out;
    if (bgr.empty()) return out;

    const auto faces = detector_.detect(bgr, minFaceSizePx_);
    out.reserve(faces.size());

    for (const auto& fr : faces) {
        FaceMatch m;
        m.rect = fr;

        cv::Mat faceBgr = cropAndNormalizeFaceBgr(bgr, fr);
        if (faceBgr.empty()) {
            m.personId = "UNKNOWN";
            m.distance = 1.0;
            m.confidence = 0.0;
            m.accepted = false;
            out.push_back(std::move(m));
            continue;
        }

        std::string err;
        auto optEmb = embedder_.embedAlignedFaceBgr(faceBgr, &err);
        if (!optEmb) {
            m.personId = "UNKNOWN";
            m.distance = 1.0;
            m.confidence = 0.0;
            m.accepted = false;
            out.push_back(std::move(m));
            continue;
        }

        auto emb = std::move(optEmb->values);
        l2NormalizeInplace(emb);

        float bestSim = -1.0f;
        std::string bestId;

        for (const auto& kv : db_.persons()) {
            auto dbEmb = kv.second.mean;
            l2NormalizeInplace(dbEmb);
            float sim = cosineSimilarity(emb, dbEmb);
            if (sim > bestSim) {
                bestSim = sim;
                bestId = kv.first;
            }
        }

        m.distance = 1.0f - bestSim;  // cosine distance
        if (bestSim >= identifyThreshold_) {
            m.personId = bestId;
            m.accepted = true;
            m.confidence = std::clamp(bestSim, 0.0f, 1.0f);
        } else {
            m.personId = "UNKNOWN";
            m.accepted = false;
            m.confidence = 0.0;
        }

        out.push_back(std::move(m));
    }

    return out;
}

bool ArcFaceWinRecognizer::enrollFromFrame(const std::string& personId, const cv::Mat& bgr, int samplesToTake, int& samplesTakenOut) {
    samplesTakenOut = 0;
    if (personId.empty() || bgr.empty()) return false;
    if (samplesToTake <= 0) return false;

    const auto faces = detector_.detect(bgr, minFaceSizePx_);
    if (faces.empty()) return false;
    const cv::Rect face = pickLargest(faces);

    cv::Mat faceBgr = cropAndNormalizeFaceBgr(bgr, face);
    if (faceBgr.empty()) return false;

    std::string err;
    auto optEmb = embedder_.embedAlignedFaceBgr(faceBgr, &err);
    if (!optEmb) return false;

    auto emb = std::move(optEmb->values);
    l2NormalizeInplace(emb);

    if (!db_.updateMean(personId, emb)) return false;
    samplesTakenOut = 1;
    return true;
}

bool ArcFaceWinRecognizer::saveDb() const {
    return db_.save(dbPath_);
}

void ArcFaceWinRecognizer::clearDb() {
    db_.clear();
}

double ArcFaceWinRecognizer::threshold() const {
    return identifyThreshold_;
}

void ArcFaceWinRecognizer::setThreshold(double t) {
    identifyThreshold_ = t;
}

std::vector<std::string> ArcFaceWinRecognizer::personIds() const {
    return db_.listPersonIds();
}

}  // namespace rk_win
