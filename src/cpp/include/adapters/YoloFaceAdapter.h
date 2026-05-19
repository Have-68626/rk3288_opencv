#pragma once

#include "FaceDetector.h"
#include "YoloFaceDetector.h"

#include <string>

/**
 * @brief Adapter that wraps YoloFaceDetector to implement the unified FaceDetector interface.
 *
 * Supports both OpenCvDnn and ncnn backends. The backend is selected via
 * the modelPath format or an explicit suffix in the load() call.
 */
class YoloFaceAdapter : public FaceDetector {
public:
    YoloFaceAdapter();
    ~YoloFaceAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    FaceDetections detect(const cv::Mat& bgr, std::string& err) override;
    const char* name() const override;

private:
    std::unique_ptr<YoloFaceDetector> inner_;
    std::string currentName_;
    YoloFaceOptions opt_;
    bool loaded_ = false;
};
