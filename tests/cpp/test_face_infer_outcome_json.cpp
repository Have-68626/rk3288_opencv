#include "FaceInferOutcomeJson.h"
#include "FaceInferencePipeline.h"
#include "FaceInferPipelineData.h"
// `ArcFaceEmbedding::kDim` 的声明处（测试夹具需要造一个**真实**的 512 维向量，
// 以免再出现「embeddingOk 为真而向量为空」的不自洽夹具）。
#include "ArcFaceEmbedder.h"
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
    // ★ 2026-09-28 补齐：本夹具原先只置 `embeddingOk = true` 而**从不写** `ctx.embedding`
    //   ⇒ 造出「embeddingOk 为真但向量为空」的不自洽状态，并使下面那条 `"dim":512` 断言
    //   恰好掩盖了 `dim` 曾被硬编码的事实。现在给出真实的 512 维向量，`dim` 才是**推导**出来的。
    ctx.embedding.assign(static_cast<std::size_t>(ArcFaceEmbedding::kDim), 0.1f);
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
    // 身份字段：personId 透传 gallery 条目 id；displayName 无 native 来源，
    // 未知时省略该字段（不输出空串，避免下游误当权威值）。
    if (!contains(json, "\"personId\":\"person_1\"")) { std::cout << "personId person_1 failed: " << json << std::endl; return false; }
    if (contains(json, "\"displayName\"")) { std::cout << "displayName should be omitted: " << json << std::endl; return false; }

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

    // ★ RED→GREEN 判别锚（2026-09-28 修 `dim` 硬编码）：本用例无脸 ⇒ `computeEmbedding`
    //   在 `!ctx.hasFace` 处早退、**从不写** `ctx.embedding` ⇒ 实际维度必须是 **0**。
    //   修复前 `dim` 被硬编码为 512，会把这个「没有嵌入」的帧谎报成 512 维嵌入
    //   ⇒ 本断言在旧实现下**必红**（这是本修复唯一的判别点，勿删）。
    if (!contains(json, "\"embedding\":{\"dim\":0,")) {
        std::cout << "dim should be 0 when no embedding: " << json << std::endl;
        return false;
    }

    return true;
}

// 身份字段（personId/displayName）的边界：空 id 与需要 JSON 转义的 id。
bool test_face_infer_outcome_json_hit_identity_edges() {
    FaceInferOutcome out;
    FaceInferRequest req;
    FaceInferContext ctx;

    FaceSearchHit emptyHit;
    emptyHit.id = "";
    emptyHit.index = 3;
    emptyHit.score = 0.5f;
    ctx.hits.push_back(emptyHit);

    FaceSearchHit weirdHit;
    weirdHit.id = "a\"b\\c";  // 含双引号与反斜杠，验证既有 jsonEscape 风格
    weirdHit.index = 4;
    weirdHit.score = 0.4f;
    ctx.hits.push_back(weirdHit);

    ThresholdDecisionResult decision;
    FaceInferMetrics metrics;

    FaceInferOutcomeJsonInput in{out, req, &ctx, &decision, &metrics, 12345};
    std::string json = buildOutcomeJson(in);

    // 空 id：personId 同样为空字符串；displayName 无 native 来源，所有命中一律省略该字段。
    if (!contains(json, "\"personId\":\"\"")) { std::cout << "empty personId failed: " << json << std::endl; return false; }
    if (contains(json, "\"displayName\"")) { std::cout << "displayName should be omitted: " << json << std::endl; return false; }
    // 转义：id = a"b\c → JSON 中为 a\"b\\c
    if (!contains(json, "\"personId\":\"a\\\"b\\\\c\"")) { std::cout << "escaped personId failed: " << json << std::endl; return false; }
    // 既有字段顺序/名称不受影响
    if (!contains(json, "\"id\":\"a\\\"b\\\\c\"")) return false;
    if (!contains(json, "\"index\":4")) return false;
    if (!contains(json, "\"rank\":2")) return false;

    return true;
}
