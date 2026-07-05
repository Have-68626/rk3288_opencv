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
            cfg_.ncnnBin = (std::filesystem::path(modelPath).parent_path() / std::filesystem::path(modelPath).stem().replace_extension(".bin")).string();
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
    return ArcFaceEmbedding::kDim;
}

const char* ArcFaceAdapter::name() const {
    return currentName_.c_str();
}
