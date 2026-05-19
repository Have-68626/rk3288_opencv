#pragma once

#include "FaceDetector.h"
#include "rk_win/FaceDetector.h"

#include <string>

/**
 * @brief Adapter wrapping rk_win::FaceDetector (LBP Cascade) into the unified FaceDetector interface.
 */
class CascadeAdapter : public FaceDetector {
public:
    CascadeAdapter();
    ~CascadeAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    FaceDetections detect(const cv::Mat& bgr, std::string& err) override;
    const char* name() const override;

private:
    rk_win::FaceDetector inner_;
    bool loaded_ = false;
    std::string currentName_;
    int minFaceSizePx_ = 60;
};
