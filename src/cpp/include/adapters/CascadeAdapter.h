#pragma once

#include "FaceDetector.h"
#include "rk_win/FaceDetector.h"

#include <mutex>
#include <string>

class CascadeAdapter : public FaceDetector {
public:
    CascadeAdapter();
    ~CascadeAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    FaceDetections detect(const cv::Mat& bgr, std::string& err) override;
    const char* name() const override;

private:
    mutable std::mutex mu_;
    rk_win::FaceDetector inner_;
    bool loaded_ = false;
    std::string currentName_;
    int minFaceSizePx_ = 60;
};
