#pragma once

#include "FaceDetector.h"

#include <opencv2/objdetect.hpp>

#include <memory>
#include <string>

class YuNetAdapter : public FaceDetector {
public:
    YuNetAdapter();
    ~YuNetAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    FaceDetections detect(const cv::Mat& bgr, std::string& err) override;
    const char* name() const override;

private:
    cv::Ptr<cv::FaceDetectorYN> inner_;
    int inputW_ = 320;
    int inputH_ = 320;
    float confThreshold_ = 0.5f;
    float nmsThreshold_ = 0.3f;
    int topK_ = 5000;
    bool loaded_ = false;
    std::string currentName_;
};
