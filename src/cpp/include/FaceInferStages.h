#pragma once

#include "FaceInferPipelineData.h"
#include "FaceInferencePipeline.h"
#include "ThresholdPolicy.h"

#include <opencv2/core.hpp>

#include <string>
#include <vector>

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

    static std::vector<float> makeFakeEmbedding512ForTest(const cv::Rect2f& bbox, const cv::Size& imgSz);
};

