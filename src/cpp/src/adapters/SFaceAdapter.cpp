#include "adapters/SFaceAdapter.h"

using namespace rk_core;

SFaceAdapter::SFaceAdapter() = default;
SFaceAdapter::~SFaceAdapter() = default;

bool SFaceAdapter::load(const std::string& modelPath, std::string& err) {
    loaded_ = false;
    inner_.release();

    inner_ = cv::FaceRecognizerSF::create(modelPath, "");
    if (inner_.empty()) {
        err = "SFaceAdapter: FaceRecognizerSF::create failed for " + modelPath;
        return false;
    }

    currentName_ = "sface";
    loaded_ = true;
    return true;
}

std::optional<std::vector<float>> SFaceAdapter::embed(const cv::Mat& alignedFaceBgr, std::string& err) {
    if (!loaded_) {
        err = "sface_not_loaded";
        return std::nullopt;
    }

    cv::Mat features;
    inner_->feature(alignedFaceBgr, features);

    if (features.empty()) {
        err = "SFaceAdapter: feature extraction returned empty";
        return std::nullopt;
    }

    std::vector<float> embedding(128);
    std::memcpy(embedding.data(), features.ptr<float>(), 128 * sizeof(float));
    return embedding;
}

const char* SFaceAdapter::name() const {
    return currentName_.c_str();
}
