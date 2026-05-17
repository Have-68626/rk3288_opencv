#pragma once

#include "FaceInferPipelineData.h"
#include "FaceInferencePipeline.h"
#include "ThresholdPolicy.h"

#include <string>

struct FaceInferOutcomeJsonInput {
    const FaceInferOutcome& out;
    const FaceInferRequest& req;
    const FaceInferContext* ctx = nullptr;
    const ThresholdDecisionResult* decision = nullptr;
    const FaceInferMetrics* metrics = nullptr;
    long long timestampMs = 0;
};

std::string buildImageLoadFailureJson(const FaceInferOutcomeJsonInput& in);
std::string buildOutcomeJson(const FaceInferOutcomeJsonInput& in);
std::string buildExceptionJson(const FaceInferOutcomeJsonInput& in);

