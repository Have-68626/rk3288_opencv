#pragma once

#include "FaceDetector.h"
#include "rk_win/DnnSsdFaceDetector.h"

#include <mutex>
#include <string>

namespace rk_core {

class DnnSsdAdapter : public FaceDetector {
public:
    DnnSsdAdapter();
    ~DnnSsdAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    FaceDetections detect(const cv::Mat& bgr, std::string& err) override;
    const char* name() const override;

private:
    mutable std::mutex mu_;
    rk_win::DnnSsdFaceDetector inner_;
    rk_win::DnnSsdConfig cfg_;
    bool loaded_ = false;
    std::string currentName_;
};

} // namespace rk_core
