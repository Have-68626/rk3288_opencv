#pragma once

#include "Embedder.h"
#include "rk_win/LbphEmbedder.h"

#include <mutex>
#include <string>

namespace rk_core {

class LbphAdapter : public Embedder {
public:
    LbphAdapter();
    ~LbphAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    std::optional<std::vector<float>> embed(const cv::Mat& alignedFaceBgr, std::string& err) override;
    int embeddingDim() const override;
    const char* name() const override;

private:
    mutable std::mutex mu_;
    rk_win::LbphEmbedder inner_;
    bool loaded_ = false;
    std::string currentName_;
};

} // namespace rk_core
