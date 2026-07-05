#pragma once

#include "Embedder.h"

#include <mutex>
#include <string>

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
#include <net.h>
#endif

namespace rk_core {

class MobileFaceNetAdapter : public Embedder {
public:
    MobileFaceNetAdapter();
    ~MobileFaceNetAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    std::optional<std::vector<float>> embed(const cv::Mat& alignedFaceBgr, std::string& err) override;
    int embeddingDim() const override { return 128; }
    const char* name() const override;

private:
    mutable std::mutex mu_;
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    ncnn::Net net_;
#endif
    bool loaded_ = false;
    std::string currentName_;
    int inputW_ = 112;
    int inputH_ = 112;
};

} // namespace rk_core
