#include "adapters/CascadeAdapter.h"

#include "NativeLog.h"

CascadeAdapter::CascadeAdapter() = default;
CascadeAdapter::~CascadeAdapter() = default;

bool CascadeAdapter::load(const std::string& modelPath, std::string& err) {
    loaded_ = false;
    if (!inner_.loadCascade(modelPath)) {
        err = "cascade_load_failed";
        return false;
    }
    currentName_ = "cascade_lbp";
    loaded_ = true;
    return true;
}

FaceDetections CascadeAdapter::detect(const cv::Mat& bgr, std::string& err) {
    if (!loaded_) {
        err = "cascade_not_loaded";
        return {};
    }
    auto rects = inner_.detect(bgr, minFaceSizePx_);
    FaceDetections dets;
    dets.reserve(rects.size());
    for (const auto& r : rects) {
        FaceDetection d;
        d.bbox = cv::Rect2f(static_cast<float>(r.x),
                             static_cast<float>(r.y),
                             static_cast<float>(r.width),
                             static_cast<float>(r.height));
        d.score = 1.0f;
        dets.push_back(std::move(d));
    }
    return dets;
}

const char* CascadeAdapter::name() const {
    return currentName_.c_str();
}
