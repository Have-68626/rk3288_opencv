#include "adapters/LbphAdapter.h"

#include <opencv2/imgproc.hpp>

LbphAdapter::LbphAdapter() = default;
LbphAdapter::~LbphAdapter() = default;

bool LbphAdapter::load(const std::string& modelPath, std::string& err) {
    (void)modelPath;
    loaded_ = true;
    currentName_ = "lbph";
    return true;
}

std::optional<std::vector<float>> LbphAdapter::embed(const cv::Mat& alignedFaceBgr, std::string& err) {
    if (!loaded_) {
        err = "lbph_not_loaded";
        return std::nullopt;
    }
    cv::Mat gray;
    if (alignedFaceBgr.channels() == 3) {
        cv::cvtColor(alignedFaceBgr, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = alignedFaceBgr;
    }
    return inner_.embedFaceGray(gray);
}

int LbphAdapter::embeddingDim() const {
    return 0;  // variable-length histogram
}

const char* LbphAdapter::name() const {
    return currentName_.c_str();
}
