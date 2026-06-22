#pragma once

#include "Embedder.h"

#include <string>

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
#include <net.h>
#endif

/**
 * @brief Adapter wrapping a MobileFaceNet ncnn model into the unified Embedder interface.
 *
 * MobileFaceNet is a lightweight face recognition model (~3.5MB FP32) that
 * produces 128-dimensional embeddings. It runs ~2-3x faster than ArcFace 512D
 * on RK3288 and is suitable for resource-constrained devices.
 *
 * Requires .param/.bin ncnn model files to be downloaded separately.
 */
class MobileFaceNetAdapter : public Embedder {
public:
    MobileFaceNetAdapter();
    ~MobileFaceNetAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    std::optional<std::vector<float>> embed(const cv::Mat& alignedFaceBgr, std::string& err) override;
    int embeddingDim() const override { return 128; }
    const char* name() const override { return currentName_.c_str(); }

private:
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    ncnn::Net net_;
#endif
    bool loaded_ = false;
    std::string currentName_;
    int inputW_ = 112;
    int inputH_ = 112;
};
