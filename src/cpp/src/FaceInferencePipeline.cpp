#if __has_include("FaceInferencePipeline.h")
#include "FaceInferencePipeline.h"
#else
#include "../include/FaceInferencePipeline.h"
#endif

#include "NativeLog.h"

#if __has_include(<opencv2/core.hpp>) && __has_include(<opencv2/imgcodecs.hpp>)
#define RK_CPP_HAS_OPENCV 1
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#else
#define RK_CPP_HAS_OPENCV 0
#endif

#include <chrono>
#include <sstream>
#include <vector>

#if RK_CPP_HAS_OPENCV
#include "FaceInferOutcomeJson.h"
#include "FaceInferPipelineData.h"
#include "FaceInferStages.h"
#endif

namespace {

static long long nowEpochMillis() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

static std::string jsonEscapeMinimal(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) out += "?";
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

static std::string buildSimpleErrorJson(bool ok,
                                        int errorCode,
                                        const std::string& stage,
                                        const std::string& message,
                                        long long tsMs,
                                        const std::string& imagePath) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"ok\":" << (ok ? "true" : "false") << ",";
    oss << "\"errorCode\":" << errorCode << ",";
    oss << "\"stage\":\"" << jsonEscapeMinimal(stage) << "\",";
    oss << "\"message\":\"" << jsonEscapeMinimal(message) << "\",";
    oss << "\"timestamp_ms\":" << tsMs;
    if (!imagePath.empty()) {
        oss << ",\"image\":\"" << jsonEscapeMinimal(imagePath) << "\"";
    }
    oss << "}";
    return oss.str();
}

static int errorCodeForStage(const std::string& stage) {
    if (stage == "image_load") return 100;
#if !RK_CPP_HAS_OPENCV
    if (stage == "opencv_headers") return 110;
#endif
    if (stage == "yolo_load") return 200;
    if (stage == "yolo_detect") return 210;
    if (stage == "align") return 300;
    if (stage == "arc_init") return 400;
    if (stage == "arc_embed") return 410;
    if (stage == "gallery_load") return 500;
    if (stage == "search") return 510;
    if (stage == "exception") return 900;
    return 1;
}

}  // namespace

#if RK_CPP_HAS_OPENCV
FaceInferOutcome runFaceInferOnce(const FaceInferRequest& req) {
    FaceInferOutcome out;
    const long long tsMs = nowEpochMillis();
    try {
        const auto t0 = std::chrono::steady_clock::now();
        FaceInferContext ctx;
        FaceInferMetrics m;
        ctx.arcBackendName = req.arcBackend;

        static std::atomic<std::uint64_t> s_auditSeq{0};
        const auto makeAuditFilename = [&]() {
            return std::string("face_infer_") + std::to_string(tsMs) + "_" + std::to_string(s_auditSeq++) + ".json";
        };

        const auto finalizeTotalMs = [&]() {
            const auto tEnd = std::chrono::steady_clock::now();
            m.msTotal = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - t0).count();
        };

        const auto makeFailureOutcome = [&](const std::string& stage, const std::string& message) -> FaceInferOutcome {
            finalizeTotalMs();
            out.ok = false;
            out.stage = stage;
            out.message = message;
            out.errorCode = errorCodeForStage(stage);
            out.auditDir = "ErrorLog";
            out.auditFilename = makeAuditFilename();

            if (stage == "image_load") {
                FaceInferOutcomeJsonInput jin{out, req};
                jin.metrics = &m;
                jin.timestampMs = tsMs;
                out.json = buildImageLoadFailureJson(jin);
            } else {
                const ThresholdDecisionResult decision = FaceInferStages::makeDecision(req, ctx);
                FaceInferOutcomeJsonInput jin{out, req};
                jin.ctx = &ctx;
                jin.decision = &decision;
                jin.metrics = &m;
                jin.timestampMs = tsMs;
                out.json = buildOutcomeJson(jin);
            }
            return out;
        };

        const auto makeSuccessOutcome = [&](const ThresholdDecisionResult& decision) -> FaceInferOutcome {
            finalizeTotalMs();
            out.ok = true;
            out.stage = "done";
            out.message = ctx.hasFace ? "" : "no_face_detected";
            out.errorCode = 0;
            out.auditDir = "tests/metrics";
            out.auditFilename = makeAuditFilename();

            FaceInferOutcomeJsonInput jin{out, req};
            jin.ctx = &ctx;
            jin.decision = &decision;
            jin.metrics = &m;
            jin.timestampMs = tsMs;
            out.json = buildOutcomeJson(jin);
            return out;
        };

        const FaceInferStageStatus sLoad = FaceInferStages::loadImage(req, ctx, m);
        if (!sLoad.ok) {
            return makeFailureOutcome("image_load", sLoad.message.empty() ? "image_load_failed" : sLoad.message);
        }

        const FaceInferStageStatus sDetect = FaceInferStages::detectFaces(req, ctx, m);
        if (!sDetect.ok) return makeFailureOutcome(sDetect.stage, sDetect.message);

        FaceInferStages::selectMainFace(req, ctx);

        const FaceInferStageStatus sAlign = FaceInferStages::alignFace(req, ctx, m);
        if (!sAlign.ok) return makeFailureOutcome(sAlign.stage, sAlign.message);

        const FaceInferStageStatus sEmbed = FaceInferStages::computeEmbedding(req, ctx, m);
        if (!sEmbed.ok) return makeFailureOutcome(sEmbed.stage, sEmbed.message);

        const FaceInferStageStatus sGallery = FaceInferStages::loadGallery(req, ctx);
        if (!sGallery.ok) return makeFailureOutcome(sGallery.stage, sGallery.message);

        const FaceInferStageStatus sSearch = FaceInferStages::searchTopK(req, ctx, m);
        if (!sSearch.ok) return makeFailureOutcome(sSearch.stage, sSearch.message);

        const ThresholdDecisionResult decision = FaceInferStages::makeDecision(req, ctx);
        return makeSuccessOutcome(decision);
    } catch (const std::exception& e) {
        out.ok = false;
        out.stage = "exception";
        out.message = e.what();
        out.errorCode = errorCodeForStage(out.stage);
        out.auditDir = "ErrorLog";
        out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";
        FaceInferOutcomeJsonInput jin{out, req};
        jin.timestampMs = tsMs;
        out.json = buildExceptionJson(jin);
        return out;
    } catch (...) {
        out.ok = false;
        out.stage = "exception";
        out.message = "unknown_exception";
        out.errorCode = errorCodeForStage(out.stage);
        out.auditDir = "ErrorLog";
        out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";
        FaceInferOutcomeJsonInput jin{out, req};
        jin.timestampMs = tsMs;
        out.json = buildExceptionJson(jin);
        return out;
    }
}
#else
FaceInferOutcome runFaceInferOnce(const FaceInferRequest& req) {
    FaceInferOutcome out;
    const long long tsMs = nowEpochMillis();
    out.ok = false;
    out.stage = "opencv_headers";
    out.message = "opencv_headers_not_found";
    out.errorCode = errorCodeForStage(out.stage);
    out.auditDir = "ErrorLog";
    out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";
    out.json = buildSimpleErrorJson(false, out.errorCode, out.stage, out.message, tsMs, req.imagePath);
    return out;
}
#endif
