#include "adapters/DnnSsdAdapter.h"

#include "NativeLog.h"

DnnSsdAdapter::DnnSsdAdapter() {
    cfg_.inputWidth = 300;
    cfg_.inputHeight = 300;
    cfg_.scale = 1.0;
    cfg_.meanB = 104;
    cfg_.meanG = 177;
    cfg_.meanR = 123;
    cfg_.swapRB = false;
    cfg_.confThreshold = 0.50;
}

DnnSsdAdapter::~DnnSsdAdapter() = default;

bool DnnSsdAdapter::load(const std::string& modelPath, std::string& err) {
    loaded_ = false;
    inner_.shutdown();

    cfg_.modelPath = std::filesystem::path(modelPath);
    cfg_.configPath = std::filesystem::path(modelPath).replace_extension(".pbtxt");

    if (!inner_.initialize(cfg_, err)) {
        return false;
    }

    currentName_ = "dnn_ssd";
    loaded_ = true;
    inner_.resetForStream();
    return true;
}

FaceDetections DnnSsdAdapter::detect(const cv::Mat& bgr, std::string& err) {
    if (!loaded_) {
        err = "dnn_ssd_not_loaded";
        return {};
    }
    double latencyMs = 0.0;
    auto results = inner_.detect(bgr, latencyMs);

    FaceDetections dets;
    dets.reserve(results.size());
    for (const auto& r : results) {
        FaceDetection d;
        d.bbox = cv::Rect2f(static_cast<float>(r.rect.x),
                             static_cast<float>(r.rect.y),
                             static_cast<float>(r.rect.width),
                             static_cast<float>(r.rect.height));
        d.score = r.confidence;
        dets.push_back(std::move(d));
    }
    return dets;
}

const char* DnnSsdAdapter::name() const {
    return currentName_.c_str();
}
