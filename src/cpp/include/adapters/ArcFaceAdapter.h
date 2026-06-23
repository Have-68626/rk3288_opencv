#pragma once

#include "Embedder.h"
#include "ArcFaceEmbedder.h"

#include <mutex>
#include <string>

class ArcFaceAdapter : public Embedder {
public:
    ArcFaceAdapter();
    ~ArcFaceAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    std::optional<std::vector<float>> embed(const cv::Mat& alignedFaceBgr, std::string& err) override;
    int embeddingDim() const override;
    const char* name() const override;

private:
    mutable std::mutex mu_;
    ArcFaceEmbedder inner_;
    ArcFaceEmbedderConfig cfg_;
    bool loaded_ = false;
    std::string currentName_;
};
