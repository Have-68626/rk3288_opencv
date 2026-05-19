#pragma once

#include "Embedder.h"
#include "LbphEmbedder.h"

#include <string>

/**
 * @brief Adapter wrapping rk_win::LbphEmbedder into the unified Embedder interface.
 *
 * LBPH produces histogram-like vectors (not fixed-dim embeddings), so embeddingDim() returns 0.
 */
class LbphAdapter : public Embedder {
public:
    LbphAdapter();
    ~LbphAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    std::optional<std::vector<float>> embed(const cv::Mat& alignedFaceBgr, std::string& err) override;
    int embeddingDim() const override;
    const char* name() const override;

private:
    rk_win::LbphEmbedder inner_;
    bool loaded_ = false;
    std::string currentName_;
};
