#pragma once

#include "FaceDetector.h"
#include "DnnSsdFaceDetector.h"

#include <string>

/**
 * @brief Adapter wrapping rk_win::DnnSsdFaceDetector into the unified FaceDetector interface.
 */
class DnnSsdAdapter : public FaceDetector {
public:
    DnnSsdAdapter();
    ~DnnSsdAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    FaceDetections detect(const cv::Mat& bgr, std::string& err) override;
    const char* name() const override;

private:
    rk_win::DnnSsdFaceDetector inner_;
    rk_win::DnnSsdConfig cfg_;
    bool loaded_ = false;
    std::string currentName_;
};
