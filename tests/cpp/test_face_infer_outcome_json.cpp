#include "FaceInferOutcomeJson.h"
#include "FaceInferencePipeline.h"
#include "FaceInferPipelineData.h"
#include "ThresholdPolicy.h"

using namespace rk_core;

#include <string>
#include <vector>
#include <iostream>

namespace {
bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}
} // namespace

bool test_face_infer_outcome_json_missing_ctx() {
    FaceInferOutcome out;
    FaceInferRequest req;

    ThresholdDecisionResult decision;
    FaceInferMetrics metrics;

    FaceInferOutcomeJsonInput in{out, req, nullptr, &decision, &metrics, 12345};
    std::string json = buildOutcomeJson(in);

    return json == "{}";
}

bool test_face_infer_outcome_json_missing_decision() {
    FaceInferOutcome out;
    FaceInferRequest req;

    FaceInferContext ctx;
    FaceInferMetrics metrics;

    FaceInferOutcomeJsonInput in{out, req, &ctx, nullptr, &metrics, 12345};
    std::string json = buildOutcomeJson(in);

    return json == "{}";
}

bool test_face_infer_outcome_json_missing_metrics() {
    FaceInferOutcome out;
    FaceInferRequest req;

    FaceInferContext ctx;
    ThresholdDecisionResult decision;

    FaceInferOutcomeJsonInput in{out, req, &ctx, &decision, nullptr, 12345};
    std::string json = buildOutcomeJson(in);

    return json == "{}";
}

bool test_face_infer_outcome_json_full() {
    FaceInferOutcome out;
    out.ok = true;
    out.errorCode = 0;
    out.stage = "done";
    out.message = "success";

    FaceInferRequest req;
    req.imagePath = "test.jpg";
    req.arcModelVersion = 42;
    req.yoloBackend = "yolo_test_backend";
    req.fakeDetect = true;
    req.arcInputW = 112;
    req.arcInputH = 112;
    req.galleryDir = "/data/gallery";
    req.topK = 5;

    FaceInferContext ctx;
    ctx.img = cv::Mat(1080, 1920, CV_8UC3); // w=1920, h=1080
    ctx.yoloBackendName = "yolo_v5";
    ctx.hasFace = true;
    ctx.mainFace.bbox = cv::Rect2f(10.5f, 20.5f, 100.0f, 200.0f);
    ctx.mainFace.score = 0.95f;
    ctx.mainFace.keypoints5 = std::array<cv::Point2f, 5>{}; // Just needs to have value
    ctx.usedKeypoints5 = true;
    ctx.usedBboxFallback = false;
    ctx.arcBackendName = "arc_v2";
    ctx.embeddingFake = false;
    ctx.embeddingOk = true;

    FaceSearchHit hit;
    hit.id = "person_1";
    hit.index = 0;
    hit.score = 0.88f;
    ctx.hits.push_back(hit);

    ctx.galleryWarnings.push_back("Warning1");

    ThresholdDecisionResult decision;
    decision.versionId = "v1.2";
    decision.threshold = 0.8f;
    decision.passNow = true;
    decision.triggeredNow = true;
    decision.triggeredLatched = false;
    decision.passStreak = 3;
    decision.score = 0.88f;

    FaceInferMetrics metrics;
    metrics.msLoad = 10;
    metrics.msDetect = 20;
    metrics.msAlign = 5;
    metrics.msEmbed = 15;
    metrics.msSearch = 2;
    metrics.msTotal = 52;

    FaceInferOutcomeJsonInput in{out, req, &ctx, &decision, &metrics, 1234567890};
    std::string json = buildOutcomeJson(in);

    if (!contains(json, "\"ok\":true")) return false;
    if (!contains(json, "\"errorCode\":0")) return false;
    if (!contains(json, "\"stage\":\"done\"")) return false;
    if (!contains(json, "\"message\":\"success\"")) return false;
    if (!contains(json, "\"timestamp_ms\":1234567890")) return false;
    if (!contains(json, "\"image\":\"test.jpg\"")) return false;
    if (!contains(json, "\"modelVersion\":42")) return false;
    if (!contains(json, "\"thresholdVersion\":\"v1.2\"")) return false;

    // frame
    if (!contains(json, "\"frame\":{\"w\":1920,\"h\":1080}")) return false;

    // yolo
    if (!contains(json, "\"yolo\":{\"backend\":\"yolo_v5\",\"fake\":true}")) return false;

    // face
    if (!contains(json, "\"face\":{\"hasFace\":true")) return false;
    if (!contains(json, "\"bbox\":{\"x\":10.5,\"y\":20.5,\"w\":100,\"h\":200}")) return false;
    if (!contains(json, "\"score\":0.95")) return false;
    if (!contains(json, "\"hasKeypoints5\":true")) return false;
    if (!contains(json, "\"align\":{\"usedKeypoints5\":true,\"usedBboxFallback\":false,\"outW\":112,\"outH\":112}")) return false;

    // embedding
    if (!contains(json, "\"embedding\":{\"dim\":512,\"backend\":\"arc_v2\",\"fake\":false,\"modelVersion\":42,\"preprocessVersion\":1}")) return false;

    // gallery
    if (!contains(json, "\"gallery\":{\"dir\":\"/data/gallery\",\"size\":0,\"warnings\":[\"Warning1\"]}")) return false;

    // TopK
    if (!contains(json, "\"TopK\":{\"k\":5")) { std::cout << "TopK k 5 failed: " << json << std::endl; return false; }
    if (!contains(json, "\"rank\":1")) { std::cout << "rank 1 failed: " << json << std::endl; return false; }
    if (!contains(json, "\"id\":\"person_1\"")) { std::cout << "id person_1 failed: " << json << std::endl; return false; }
    if (!contains(json, "\"index\":0")) { std::cout << "index 0 failed: " << json << std::endl; return false; }
    if (!contains(json, "\"score\":0.88")) { std::cout << "score 0.88 failed: " << json << std::endl; return false; }

    // decision
    if (!contains(json, "\"decision\":{\"threshold\":0.8")) { std::cout << "decision threshold failed: " << json << std::endl; return false; }
    if (!contains(json, "\"thresholdVersion\":\"v1.2\"")) { std::cout << "decision thresholdVersion failed: " << json << std::endl; return false; }
    if (!contains(json, "\"passNow\":true")) { std::cout << "decision passNow failed: " << json << std::endl; return false; }
    if (!contains(json, "\"triggeredNow\":true")) { std::cout << "decision triggeredNow failed: " << json << std::endl; return false; }
    if (!contains(json, "\"triggeredLatched\":false")) { std::cout << "decision triggeredLatched failed: " << json << std::endl; return false; }
    if (!contains(json, "\"passStreak\":3")) { std::cout << "decision passStreak failed: " << json << std::endl; return false; }
    if (!contains(json, "\"bestScore\":0.88")) { std::cout << "decision bestScore failed: " << json << std::endl; return false; }

    // metrics
    if (!contains(json, "\"metrics\":{\"msLoad\":10,\"msDetect\":20,\"msAlign\":5,\"msEmbed\":15,\"msSearch\":2,\"msTotal\":52}")) { std::cout << "metrics failed: " << json << std::endl; return false; }

    return true;
}

bool test_face_infer_outcome_json_no_face() {
    FaceInferOutcome out;
    FaceInferRequest req;
    FaceInferContext ctx;
    ctx.hasFace = false;

    ThresholdDecisionResult decision;
    FaceInferMetrics metrics;

    FaceInferOutcomeJsonInput in{out, req, &ctx, &decision, &metrics, 12345};
    std::string json = buildOutcomeJson(in);

    if (!contains(json, "\"face\":{\"hasFace\":false}")) return false;

    return true;
}
