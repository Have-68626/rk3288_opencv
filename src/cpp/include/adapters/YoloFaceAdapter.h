#pragma once

#include "FaceDetector.h"
#include "YoloFaceDetector.h"

#include <mutex>
#include <string>

namespace rk_core {

class YoloFaceAdapter : public FaceDetector {
public:
    YoloFaceAdapter();
    ~YoloFaceAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    FaceDetections detect(const cv::Mat& bgr, std::string& err) override;
    const char* name() const override;

private:
    mutable std::mutex mu_;
    std::unique_ptr<YoloFaceDetector> inner_;
    std::string currentName_;
    YoloFaceOptions opt_;
    bool loaded_ = false;
};

} // namespace rk_core
