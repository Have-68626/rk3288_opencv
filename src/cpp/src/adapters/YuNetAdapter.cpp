#include "adapters/YuNetAdapter.h"

using namespace rk_core;

YuNetAdapter::YuNetAdapter() = default;
YuNetAdapter::~YuNetAdapter() = default;

bool YuNetAdapter::load(const std::string& modelPath, std::string& err) {
    loaded_ = false;
    inner_.release();

    inner_ = cv::FaceDetectorYN::create(
        modelPath,
        "",
        cv::Size(inputW_, inputH_),
        confThreshold_,
        nmsThreshold_,
        topK_
    );
    if (inner_.empty()) {
        err = "YuNetAdapter: FaceDetectorYN::create failed for " + modelPath;
        return false;
    }

    currentName_ = "yunet";
    loaded_ = true;
    return true;
}

FaceDetections YuNetAdapter::detect(const cv::Mat& bgr, std::string& err) {
    if (!loaded_) {
        err = "yunet_not_loaded";
        return {};
    }

    cv::Mat faces;
    inner_->detect(bgr, faces);

    FaceDetections dets;
    if (faces.empty()) return dets;

    dets.reserve(faces.rows);
    for (int i = 0; i < faces.rows; i++) {
        const float* row = faces.ptr<float>(i);
        float x1 = row[0];
        float y1 = row[1];
        float w = row[2];
        float h = row[3];
        float score = row[14];

        if (score < confThreshold_) continue;

        FaceDetection d;
        d.bbox = cv::Rect2f(x1, y1, w, h);
        d.score = score;
        dets.push_back(std::move(d));
    }
    return dets;
}

const char* YuNetAdapter::name() const {
    return currentName_.c_str();
}
