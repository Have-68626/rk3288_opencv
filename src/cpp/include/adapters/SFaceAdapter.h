#pragma once

#include "Embedder.h"

#include <opencv2/face.hpp>

#include <memory>
#include <mutex>
#include <string>

class SFaceAdapter : public Embedder {
public:
    SFaceAdapter();
    ~SFaceAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    std::optional<std::vector<float>> embed(const cv::Mat& alignedFaceBgr, std::string& err) override;
    int embeddingDim() const override { return 128; }
    const char* name() const override;

private:
    mutable std::mutex mu_;
    cv::Ptr<cv::FaceRecognizerSF> inner_;
    int inputW_ = 112;
    int inputH_ = 112;
    bool loaded_ = false;
    std::string currentName_;
};
