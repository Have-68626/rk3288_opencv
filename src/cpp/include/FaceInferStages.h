#pragma once

#include "FaceInferPipelineData.h"
#include "FaceInferencePipeline.h"
#include "ThresholdPolicy.h"

#if __has_include(<opencv2/core.hpp>) && !defined(RK_SKIP_OPENCV)
#include <opencv2/core.hpp>
#else
namespace cv { class Mat; }
#endif

#include <string>
#include <vector>

namespace rk_core {

struct FaceInferStageStatus {
    bool ok = true;
    std::string stage;
    std::string message;
};

class FaceInferStages {
public:
    static FaceInferStageStatus loadImage(const FaceInferRequest& req, FaceInferContext& ctx, FaceInferMetrics& m);
    static FaceInferStageStatus detectFaces(const FaceInferRequest& req, FaceInferContext& ctx, FaceInferMetrics& m);
    static void selectMainFace(const FaceInferRequest& req, FaceInferContext& ctx);
    static FaceInferStageStatus alignFace(const FaceInferRequest& req, FaceInferContext& ctx, FaceInferMetrics& m);
    static FaceInferStageStatus computeEmbedding(const FaceInferRequest& req, FaceInferContext& ctx, FaceInferMetrics& m);
    static FaceInferStageStatus loadGallery(const FaceInferRequest& req, FaceInferContext& ctx);
    static FaceInferStageStatus searchTopK(const FaceInferRequest& req, FaceInferContext& ctx, FaceInferMetrics& m);
    static ThresholdDecisionResult makeDecision(const FaceInferRequest& req, const FaceInferContext& ctx);

    /** Generate a deterministic fake 512-dim embedding for test/debug mode (used when req.fakeEmbedding == true) */
    static std::vector<float> makeFakeEmbedding512(const cv::Rect2f& bbox, const cv::Size& imgSz);
};

} // namespace rk_core

