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

using namespace rk_core;

#include <chrono>
#include <fstream>
#include <sstream>
#include <vector>

#if RK_CPP_HAS_OPENCV
#include "ArcFaceEmbedder.h"
#include "Embedder.h"
#include "FaceDetector.h"
#include "FaceInferOutcomeJson.h"
#include "FaceInferPipelineData.h"
#include "FaceInferStages.h"
#include "FaceProfileRequest.h"
#include "ModelRegistry.h"
#include "NcnnPrecisionGuard.h"
#include "adapters/ArcFaceAdapter.h"
#include "adapters/ScrfdNcnnAdapter.h"
#include "adapters/YoloFaceAdapter.h"
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
        else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", static_cast<int>(c));
            out += buf;
        }
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
    std::string out;
    const std::string escStage = jsonEscapeMinimal(stage);
    const std::string escMsg = jsonEscapeMinimal(message);
    const std::string escImg = !imagePath.empty() ? jsonEscapeMinimal(imagePath) : "";
    out.reserve(128 + escStage.size() + escMsg.size() + escImg.size());
    out += "{";
    out += "\"ok\":";
    out += (ok ? "true" : "false");
    out += ",\"errorCode\":";
    out += std::to_string(errorCode);
    out += ",\"stage\":\"";
    out += escStage;
    out += "\",\"message\":\"";
    out += escMsg;
    out += "\",\"timestamp_ms\":";
    out += std::to_string(tsMs);
    if (!imagePath.empty()) {
        out += ",\"image\":\"";
        out += escImg;
        out += "\"";
    }
    out += "}";
    return out;
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
    if (stage == "face_profile") return 120;
    if (stage == "exception") return 900;
    return 1;
}



// 当前进程编译 ABI 字符串（用于 profile 的 supportedAbis 校验）。
// 与 §I5 单图 JNI 同一口径：按**编译期**宏判断，不以设备列表/运行态 ABI 代替。
static std::string currentAbiString() {
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__arm__)
    return "armeabi-v7a";
#elif defined(_M_X64) || defined(__x86_64__)
    return "windows-x64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

}  // namespace

namespace rk_core {

#if RK_CPP_HAS_OPENCV
FaceInferOutcome runFaceInferOnce(const FaceInferRequest& reqIn) {
    FaceInferOutcome out;
    const long long tsMs = nowEpochMillis();
    try {
        // 单帧没有跨调用状态，不能接受一个永远无法累计完成的连续帧要求。
        if (reqIn.consecutivePassesToTrigger > 1) {
            out.stage = "request";
            out.message = "single_frame_consecutive_requires_one";
            out.errorCode = errorCodeForStage(out.stage);
            out.auditDir = "ErrorLog";
            out.auditFilename = "face_infer_" + std::to_string(tsMs) + ".json";
            out.json = buildSimpleErrorJson(false, out.errorCode, out.stage, out.message, tsMs, reqIn.imagePath);
            return out;
        }
        // §I3：profile 模式在装配前一次性派生请求字段（detectorId/embedderId/双后端/模型绝对路径/节点/
        // 尺寸/阈值/int8Enabled）并取回 profile 供 SCRFD 解码语义消费。失败时 req 不变、直接失败。
        FaceInferRequest req = reqIn;
        FaceRuntimeProfile profile;
        bool haveProfile = false;
        if (!req.faceProfileDir.empty()) {
            FaceInferRequest eff = req;
            std::string perr;
            if (!applyFaceProfileToRequest(req.faceProfileDir, currentAbiString(), eff, profile, perr)) {
                out.ok = false;
                out.stage = "face_profile";
                out.message = perr.empty() ? "face_profile_failed" : perr;
                out.errorCode = errorCodeForStage(out.stage);
                out.auditDir = "ErrorLog";
                out.auditFilename = std::string("face_infer_") + std::to_string(tsMs) + ".json";
                out.json = buildSimpleErrorJson(false, out.errorCode, out.stage, out.message, tsMs, reqIn.imagePath);
                return out;
            }
            req = eff;
            haveProfile = true;
        }

        if (req.int8Enabled) {
            const auto precisionFailure = [&](const char* stage, const std::string& message) {
                out.stage = stage;
                out.message = message;
                out.errorCode = errorCodeForStage(out.stage);
                out.auditDir = "ErrorLog";
                out.auditFilename = "face_infer_" + std::to_string(tsMs) + ".json";
                out.json = buildSimpleErrorJson(false, out.errorCode, out.stage, message, tsMs, req.imagePath);
                return out;
            };
            std::string precisionErr;
            if (!req.fakeDetect && !satisfiesInt8Requirement(req.yoloBackend,
                    req.yoloNcnnParam.empty() ? req.yoloModelPath : req.yoloNcnnParam, precisionErr)) {
                return precisionFailure("yolo_load", precisionErr);
            }
            if (!req.fakeEmbedding && !satisfiesInt8Requirement(req.arcBackend,
                    req.arcNcnnParam.empty() ? req.arcModelPath : req.arcNcnnParam, precisionErr)) {
                return precisionFailure("arc_init", precisionErr);
            }
        }

        const auto t0 = std::chrono::steady_clock::now();
        FaceInferContext ctx;
        FaceInferMetrics m;
        ctx.arcBackendName = req.arcBackend;

        // 调用期局部共享所有权：即使并发切换缓存 key，本帧持有的 detector/embedder 不被销毁。
        // 必须与 ctx 同生命周期（覆盖 loadImage→detectFaces→computeEmbedding 全程）——不能声明在
        // 下方 if 块内，否则块末即析构，ctx.detector/embedder 裸指针仅靠单槽缓存持有，
        // 并发切 key 会销毁使用中的模型（UAF）。
        std::shared_ptr<FaceDetector> localDet;
        std::shared_ptr<Embedder> localEmb;

        // §I3：所有装配/缓存集中在 runFaceInferOnce；Stages 只消费注入实例或返回装配错误。
        // 检测器按 detectorId 路由（不再 createDetector(yoloBackend)）；缓存 key 含完整相关
        // ID/后端/spec/节点/尺寸/阈值/precision；profile 另含标识、四文件哈希与完整解码契约。
        if (!req.fakeDetect) {
            static std::mutex s_detMu;
            static std::string s_detCacheKey;
            static std::shared_ptr<FaceDetector> s_cachedDet;

            // key 须覆盖全部参与装配的字段：除 ID/后端/节点/尺寸/阈值/precision 外，还含
            // OpenCV-DNN 的 spec（configPath/framework/outputName）、预处理（letterbox/swapRB/
            // scale/mean/openCvBackend/openCvTarget）与 ncnn 运行时（threads/lightmode）——它们
            // 都注入到装配实例（见下方 configureOpenCv/configureNcnn），漏掉会让「仅这些字段不同」
            // 的两次请求静默复用首个实例（错配）。
            std::string detKey = req.detectorId + "|" + req.yoloBackend + "|" + req.yoloModelPath +
                                 "|p:" + req.yoloNcnnParam + "|b:" + req.yoloNcnnBin +
                                 "|in:" + req.yoloNcnnInput + "|out:" + req.yoloNcnnOutput +
                                 "|" + std::to_string(req.yoloInputW) + "x" + std::to_string(req.yoloInputH) +
                                 "|s" + std::to_string(req.yoloScoreThreshold) +
                                 "|n" + std::to_string(req.yoloNmsIouThreshold) +
                                 "|kps:" + (req.yoloEnableKeypoints5 ? "1" : "0") +
                                 "|i8:" + (req.int8Enabled ? "1" : "0") +
                                 "|cfg:" + req.yoloConfigPath + "|fw:" + req.yoloFramework +
                                 "|outn:" + req.yoloOutputName +
                                 "|lb:" + (req.yoloLetterbox ? "1" : "0") +
                                 "|swrb:" + (req.yoloSwapRB ? "1" : "0") +
                                 "|sc:" + std::to_string(req.yoloScale) +
                                 "|mb:" + std::to_string(req.yoloMeanB) + "," + std::to_string(req.yoloMeanG) +
                                 "," + std::to_string(req.yoloMeanR) +
                                 "|cvb:" + std::to_string(req.yoloOpenCvBackend) +
                                 "|cvt:" + std::to_string(req.yoloOpenCvTarget) +
                                 "|thr:" + std::to_string(req.yoloNcnnThreads) +
                                 "|lm:" + (req.yoloNcnnLightmode ? "1" : "0");
            if (haveProfile) {
                detKey += "|prof:" + profile.profileId + ":" + profile.detectorParam.sha256 + ":" +
                          profile.detectorBin.sha256 + ":" + profile.embedderParam.sha256 + ":" +
                          profile.embedderBin.sha256;
                detKey += "|decode:" + std::to_string(static_cast<int>(profile.detectorDecodeMode)) +
                          "|anchors:" + std::to_string(profile.detectorAnchorsPerPos[0]) + "," +
                          std::to_string(profile.detectorAnchorsPerPos[1]) + "," +
                          std::to_string(profile.detectorAnchorsPerPos[2]) +
                          "|layout:" + std::to_string(static_cast<int>(profile.detectorOutputLayout));
                // 节点名带长度，避免名字中的分隔符让不同输出清单碰撞。
                for (const auto& node : profile.detectorOutputs) {
                    detKey += "|node:" + std::to_string(node.size()) + ":" + node;
                }
            }
            {
                std::lock_guard<std::mutex> lock(s_detMu);
                if (detKey == s_detCacheKey && s_cachedDet) localDet = s_cachedDet;
            }
            if (!localDet) {
                ModelRegistry::ensureBuiltinRegistered();
                std::unique_ptr<FaceDetector> det;
                std::string assembleErr;

                if (req.detectorId == "scrfd_0.5gf") {
                    // 无 profile 的真实 SCRFD 不能猜 distance/anchor/节点 ⇒ 明确失败。
                    if (!haveProfile) {
                        assembleErr = "scrfd_requires_profile";
                    } else {
                        std::string cfgErr;
                        ScrfdNcnnConfig scfg = ScrfdNcnnAdapter::configFromProfile(
                            profile, req.yoloNcnnParam, req.yoloNcnnBin, cfgErr);
                        if (!cfgErr.empty()) {
                            assembleErr = cfgErr;
                        } else {
                            det = std::make_unique<ScrfdNcnnAdapter>(scfg);
                            std::string loadErr;
                            if (!det->load(req.yoloModelPath, loadErr)) {
                                assembleErr = loadErr.empty() ? "scrfd_load_failed" : loadErr;
                                det.reset();
                            }
                        }
                    }
                } else {
                    // yolo_face 及其它已注册 ID：经注册表按 ID 取适配器（未知 ID ⇒ 明确失败）。
                    std::string crErr;
                    det = ModelRegistry::instance().createDetector(req.detectorId, &crErr);
                    if (!det) {
                        assembleErr = crErr.empty() ? ("detector_not_found: " + req.detectorId) : crErr;
                    } else {
                        // YoloFaceAdapter：按后端名显式配置（不按扩展名猜后端）。
                        if (auto* yolo = dynamic_cast<YoloFaceAdapter*>(det.get())) {
                            const std::string& be = req.yoloBackend;
                            YoloFaceOptions opt;
                            opt.inputW = req.yoloInputW;
                            opt.inputH = req.yoloInputH;
                            opt.scoreThreshold = req.yoloScoreThreshold;
                            opt.nmsIouThreshold = req.yoloNmsIouThreshold;
                            opt.enableKeypoints5 = req.yoloEnableKeypoints5;
                            opt.letterbox = req.yoloLetterbox;
                            opt.swapRB = req.yoloSwapRB;
                            opt.scale = req.yoloScale;
                            opt.meanB = req.yoloMeanB;
                            opt.meanG = req.yoloMeanG;
                            opt.meanR = req.yoloMeanR;
                            opt.opencvBackend = req.yoloOpenCvBackend;
                            opt.opencvTarget = req.yoloOpenCvTarget;
                            if (be == "opencv" || be == "opencv_dnn" || be == "qualcomm") {
                                // qualcomm 无独立实现，历史即回落 CPU（OpenCV）⇒ 与 opencv 同装配。
                                YoloFaceModelSpec spec;
                                spec.modelPath = req.yoloModelPath;
                                spec.configPath = req.yoloConfigPath;
                                spec.framework = req.yoloFramework;
                                spec.outputName = req.yoloOutputName;
                                yolo->configureOpenCv(spec, opt);
                            } else if (be == "ncnn") {
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
                                NcnnYoloFaceModelSpec ns;
                                ns.paramPath = req.yoloNcnnParam.empty() ? req.yoloModelPath : req.yoloNcnnParam;
                                ns.binPath = req.yoloNcnnBin;
                                ns.inputName = req.yoloNcnnInput;
                                ns.outputName = req.yoloNcnnOutput;
                                ns.threads = req.yoloNcnnThreads;
                                ns.lightmode = req.yoloNcnnLightmode;
                                yolo->configureNcnn(ns, opt);
#else
                                assembleErr = "ncnn_backend_unavailable";
#endif
                            } else {
                                assembleErr = "yolo_backend_unsupported: " + be;
                            }
                        }
                        if (assembleErr.empty()) {
                            std::string loadErr;
                            if (!det->load(req.yoloModelPath, loadErr)) {
                                assembleErr = loadErr.empty() ? "yolo_load_failed" : loadErr;
                                det.reset();
                            }
                        } else {
                            det.reset();
                        }
                    }
                }

                if (det) {
                    std::lock_guard<std::mutex> lock(s_detMu);
                    s_detCacheKey = detKey;
                    localDet = std::shared_ptr<FaceDetector>(std::move(det));
                    s_cachedDet = localDet;
                } else {
                    ctx.detectorLoadError = assembleErr.empty() ? "detector_unavailable" : assembleErr;
                }
            }
            ctx.detector = localDet.get();
        }

        // 嵌入器按 embedderId 路由；未知 ID ⇒ 明确失败。绝不把同一文件依次交给 ArcFace、MobileFaceNet
        // （无回落候选队列），显式 MobileFaceNet 仅在其自己的 embedderId 下调用。
        if (!req.fakeEmbedding) {
            static std::mutex s_embMu;
            static std::string s_embCacheKey;
            static std::shared_ptr<Embedder> s_cachedEmb;

            // key 须覆盖全部参与装配的字段：除 ID/后端/节点/尺寸/版本/precision 外，还含
            // OpenCV-DNN 的 spec（configPath/framework/outputName/inputName）、预处理版本
            // （preprocessVersion）与 ncnn 运行时（threads/lightmode）——它们都注入到装配实例
            // （见下方 ArcFaceAdapter::configure），漏掉会让「仅这些字段不同」的两次请求静默复用
            // 首个实例（错配）。
            std::string embKey = req.embedderId + "|" + req.arcBackend + "|" + req.arcModelPath +
                                 "|p:" + req.arcNcnnParam + "|b:" + req.arcNcnnBin +
                                 "|in:" + req.arcNcnnInput + "|out:" + req.arcNcnnOutput +
                                 "|" + std::to_string(req.arcInputW) + "x" + std::to_string(req.arcInputH) +
                                 "|mv:" + std::to_string(req.arcModelVersion) +
                                 "|i8:" + (req.int8Enabled ? "1" : "0") +
                                 "|cfg:" + req.arcConfigPath + "|fw:" + req.arcFramework +
                                 "|outn:" + req.arcOutputName + "|inn:" + req.arcInputName +
                                 "|pv:" + std::to_string(req.arcPreprocessVersion) +
                                 "|thr:" + std::to_string(req.arcNcnnThreads) +
                                 "|lm:" + (req.arcNcnnLightmode ? "1" : "0");
            if (haveProfile) {
                embKey += "|prof:" + profile.profileId + ":" + profile.embedderParam.sha256 + ":" +
                          profile.embedderBin.sha256;
            }
            {
                std::lock_guard<std::mutex> lock(s_embMu);
                if (embKey == s_embCacheKey && s_cachedEmb) localEmb = s_cachedEmb;
            }
            if (!localEmb) {
                ModelRegistry::ensureBuiltinRegistered();
                std::unique_ptr<Embedder> emb;
                std::string assembleErr;
                std::string crErr;
                emb = ModelRegistry::instance().createEmbedder(req.embedderId, &crErr);
                if (!emb) {
                    assembleErr = crErr.empty() ? ("embedder_not_found: " + req.embedderId) : crErr;
                } else {
                    if (auto* arc = dynamic_cast<ArcFaceAdapter*>(emb.get())) {
                        ArcFaceEmbedderConfig cfg;
                        cfg.inputW = req.arcInputW;
                        cfg.inputH = req.arcInputH;
                        cfg.modelVersion = req.arcModelVersion;
                        cfg.preprocessVersion = req.arcPreprocessVersion;
                        cfg.opencvModel = req.arcModelPath;
                        cfg.opencvConfig = req.arcConfigPath;
                        cfg.opencvFramework = req.arcFramework;
                        cfg.opencvOutput = req.arcOutputName;
                        cfg.opencvInput = req.arcInputName;
                        const std::string& be = req.arcBackend;
                        if (be == "opencv" || be == "opencv_dnn") {
                            cfg.backend = ArcFaceEmbedderConfig::BackendType::OpenCvDnn;
                        } else if (be == "ncnn") {
                            cfg.backend = ArcFaceEmbedderConfig::BackendType::Ncnn;
                            cfg.ncnnParam = req.arcNcnnParam.empty() ? req.arcModelPath : req.arcNcnnParam;
                            cfg.ncnnBin = req.arcNcnnBin;
                            cfg.ncnnInput = req.arcNcnnInput;
                            cfg.ncnnOutput = req.arcNcnnOutput;
                            cfg.ncnnThreads = req.arcNcnnThreads;
                            cfg.ncnnLightmode = req.arcNcnnLightmode;
                        } else if (be == "qualcomm") {
                            cfg.backend = ArcFaceEmbedderConfig::BackendType::Qualcomm;
                            cfg.qualcommModel = req.arcModelPath;
                        } else {
                            assembleErr = "arc_backend_unsupported: " + be;
                        }
                        if (assembleErr.empty()) arc->configure(cfg);
                    }
                    if (assembleErr.empty()) {
                        std::string loadErr;
                        if (!emb->load(req.arcModelPath, loadErr)) {
                            assembleErr = loadErr.empty() ? "arc_init_failed" : loadErr;
                            emb.reset();
                        }
                    } else {
                        emb.reset();
                    }
                }
                if (emb) {
                    std::lock_guard<std::mutex> lock(s_embMu);
                    s_embCacheKey = embKey;
                    localEmb = std::shared_ptr<Embedder>(std::move(emb));
                    s_cachedEmb = localEmb;
                } else {
                    ctx.embedderLoadError = assembleErr.empty() ? "embedder_unavailable" : assembleErr;
                }
            }
            ctx.embedder = localEmb.get();
        }

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
        // catch 在 try 作用域外，`req`（profile 派生后的局部量）不可见 ⇒ 用入参 reqIn 取 imagePath。
        FaceInferOutcomeJsonInput jin{out, reqIn};
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
        // catch 在 try 作用域外，`req`（profile 派生后的局部量）不可见 ⇒ 用入参 reqIn 取 imagePath。
        FaceInferOutcomeJsonInput jin{out, reqIn};
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

} // namespace rk_core
