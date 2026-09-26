#include "ArcFaceAdapter.h"

#include "NativeLog.h"

using namespace rk_core;

#include <filesystem>

ArcFaceAdapter::ArcFaceAdapter() {
    cfg_.inputW = 112;
    cfg_.inputH = 112;
    cfg_.scale = 1.0f / 127.5f;
    cfg_.meanB = 127.5f;
    cfg_.meanG = 127.5f;
    cfg_.meanR = 127.5f;
    cfg_.swapRB = true;
    cfg_.modelVersion = 1;
    cfg_.preprocessVersion = 1;
}

ArcFaceAdapter::~ArcFaceAdapter() = default;

bool ArcFaceAdapter::load(const std::string& modelPath, std::string& err) {
    loaded_ = false;
    std::string ext = std::filesystem::path(modelPath).extension().string();
    if (ext == ".param" || ext == ".bin") {
        cfg_.backend = ArcFaceEmbedderConfig::BackendType::Ncnn;
        if (ext == ".param") {
            cfg_.ncnnParam = modelPath;
            // 缺口2：优先用调用方注入的 .bin 路径（manifest 声明的 embedderBin）；未注入（空串）
            // 时回落到历史推导 —— 同目录同 stem 的 replace_extension(".bin")。装配侧
            // RealtimeFacePipeline::makeDefaultRealtimePipelineHooks().loadEmbedder 会显式注入
            // manifest 声明的 bin；同 stem 场景下注入路径与推导路径**解析为同一文件**（分隔符
            // 形态在 Windows 上可能不同，但指向同一路径），故加载行为不变。
            cfg_.ncnnBin =
                !injectedBinPath_.empty()
                    ? injectedBinPath_
                    : (std::filesystem::path(modelPath).parent_path() /
                       std::filesystem::path(modelPath).stem().replace_extension(".bin"))
                          .string();
        } else {
            cfg_.ncnnBin = modelPath;
            cfg_.ncnnParam = (std::filesystem::path(modelPath).parent_path() / std::filesystem::path(modelPath).stem().replace_extension(".param")).string();
        }
        rklog::logInfo("ArcFaceAdapter", "load", "ncnn backend selected for: " + modelPath);
    } else {
        cfg_.backend = ArcFaceEmbedderConfig::BackendType::OpenCvDnn;
        cfg_.opencvModel = modelPath;
        rklog::logInfo("ArcFaceAdapter", "load", "OpenCV DNN backend selected for: " + modelPath);
    }

    if (!inner_.initialize(cfg_, &err)) {
        rklog::logWarn("ArcFaceAdapter", "load", "initialize failed: " + err);
        if (cfg_.backend == ArcFaceEmbedderConfig::BackendType::Ncnn) {
            rklog::logWarn("ArcFaceAdapter", "load",
                "ncnn backend failed; fallback requires an ONNX/OpenCV model, .param/.bin cannot be used by OpenCV DNN");
        }
        return false;
    }

    currentName_ = (cfg_.backend == ArcFaceEmbedderConfig::BackendType::Ncnn) ? "ncnn" : "opencv_dnn";
    loaded_ = true;
    return true;
}

std::optional<std::vector<float>> ArcFaceAdapter::embed(const cv::Mat& alignedFaceBgr, std::string& err) {
    if (!loaded_) {
        err = "embedder_not_loaded";
        return std::nullopt;
    }
    auto result = inner_.embedAlignedFaceBgr(alignedFaceBgr, &err);
    if (!result.has_value()) {
        return std::nullopt;
    }
    return result->values;
}

int ArcFaceAdapter::embeddingDim() const {
    // 缺口3：返回注入的期望维度（默认 kDim ⇒ 未注入时与历史逐字一致）。
    return expectedDim_;
}

const char* ArcFaceAdapter::name() const {
    return currentName_.c_str();
}

void ArcFaceAdapter::setNodeNames(const std::string& input, const std::string& output) {
    // 空串忽略：调用方未提供节点名（理论上 manifest parser 已给默认）时不清空既有值，
    // 从而保留 ArcFaceEmbedderConfig 的默认 data/output（或此前一次注入的值）。
    if (!input.empty()) {
        cfg_.ncnnInput = input;
        cfg_.opencvInput = input;
    }
    if (!output.empty()) {
        cfg_.ncnnOutput = output;
        cfg_.opencvOutput = output;
    }
}

const std::string& ArcFaceAdapter::inputNodeName() const {
    // ncnn 与 opencv 节点名由 setNodeNames 同步写入，取其一即可代表当前注入值。
    return cfg_.ncnnInput;
}

const std::string& ArcFaceAdapter::outputNodeName() const {
    return cfg_.ncnnOutput;
}

void ArcFaceAdapter::setModelBinPath(const std::string& binPath) {
    // 空串忽略：未提供 manifest bin 的调用方保留 load() 的历史 stem 推导路径。
    if (!binPath.empty()) {
        injectedBinPath_ = binPath;
    }
}

void ArcFaceAdapter::setExpectedEmbeddingDim(int dim) {
    // 非法值（<=0）忽略，保留既有期望维度（默认 kDim）；与 MobileFaceNetAdapter 同语义。
    if (dim > 0) {
        expectedDim_ = dim;
    }
}
