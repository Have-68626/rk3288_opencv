#include "rk_win/RuntimeBootstrap.h"
#include "rk_win/IRecognizer.h"
#include "rk_win/FaceRecognizer.h"
#include "rk_win/ArcFaceWinRecognizer.h"
#include "rk_win/DnnSsdFaceDetector.h"
#include "rk_win/WinConfig.h"
#include "FileHash.h"

#include <filesystem>

namespace rk_win {

// 模型文件 SHA256 计算：calculateSHA256 在文件缺失或读取失败时返回空串，
// 若原样写入快照 hash，会让模型变更检测被静默绕过（空值既不匹配也无人感知）。
// 统一以明确占位符 "(unavailable)" 替代空串，保证消费方总能拿到非空值、感知“模型不可用”。
static std::string modelSha256OrUnavailable(const std::filesystem::path& p) {
    const std::string sha = rk_wcfr::calculateSHA256(p);
    return sha.empty() ? std::string("(unavailable)") : sha;
}

BootstrapResult::~BootstrapResult() = default;

BootstrapResult RuntimeBootstrap::build(const AppConfig& cfg) {
    BootstrapResult result;

    const std::string cascadeUtf8 = cfg.recognition.cascadePath.string();

    // --- 构造识别器（ArcFace 或 LBPH）---
    // recognizerOk 承载的是**识别器整体初始化成败**，并不只是 cascade 加载：ArcFace 分支还含
    // embedder（模型）加载、initialize() 末尾的初始化自检（节点名 / 模型）以及 db 加载；
    // LBPH 分支含 cascade 加载与 db 加载。旧名 cascadeOk 会误导读者以为失败仅与 cascade 有关。
    bool recognizerOk = false;
    if (cfg.model.recognition == "arcface") {
        auto arcRec = std::make_unique<ArcFaceWinRecognizer>();
        // 节点名注入必须在 initialize() 之前（initialize 内部才把 cfg 交给 embedder_）。
        // 配置缺省为空串 ⇒ 走 OpenCV 自动默认输入 / 首个输出（FACE-ARCWIN-001 的修复）。
        arcRec->setNodeNames(cfg.recognition.arcFaceInputName, cfg.recognition.arcFaceOutputName);
        // ArcFace 的判定量纲是**余弦相似度**（∈[-1,1]），与 LBPH 的卡方距离无关，
        // 故这里必须注入 identifyThresholdArcFace，绝不能复用 LBPH 专用的 identifyThreshold
        // （否则 bestSim >= 55.0 恒假 ⇒ identify() 恒返回 UNKNOWN，即 FACE-ARCWIN-006）。
        recognizerOk = arcRec->initialize(
            cascadeUtf8,
            cfg.recognition.databasePath,
            cfg.recognition.arcFaceModelPath.string(),
            cfg.recognition.minFaceSizePx,
            cfg.recognition.identifyThresholdArcFace);
        result.recognizer = std::move(arcRec);
    } else {
        auto lbpRec = std::make_unique<FaceRecognizer>();
        // LBPH 的判定量纲是**卡方距离**（约 [0,64]），沿用 identifyThreshold（默认 55.0）。
        recognizerOk = lbpRec->initialize(
            cascadeUtf8,
            cfg.recognition.databasePath,
            cfg.recognition.minFaceSizePx,
            cfg.recognition.identifyThreshold);
        result.recognizer = std::move(lbpRec);
    }

    // --- Cascade ModelSnapshot ---
    {
        ModelSnapshot m;
        m.id = "cascade_frontalface";
        m.displayName = "Cascade Frontal Face (LBP)";
        m.taskType = "detect_recognize_pipeline";
        m.configuredPath = cascadeUtf8;
        m.resolvedPath = cascadeUtf8;
        m.backend = cfg.model.recognition == "arcface" ? "opencv_dnn_arcface" : "opencv_cascade";
        // 快照语义本就是 cascade 文件（id/path 均指向 cascadePath），
        // 因此 hash 也必须取 cascadePath，与 id/path 保持一致，否则
        // FramePipeline 的 cascade 快照 hash 断言会误报 mismatch。
        m.hash = modelSha256OrUnavailable(cfg.recognition.cascadePath);
        m.status = recognizerOk ? "loaded" : "failed";
        m.isInUse = !cfg.dnn.enable;
        if (!recognizerOk) {
            // 归因如实：失败可能来自 cascade 文件、ArcFace 识别模型 / 节点名，或 database 路径，
            // 此处无法区分，故不再断言是 cascade / database 的问题（识别器自身已把原因打到日志）。
            m.lastError = "初始化失败（cascade 文件 / 识别模型（含节点名）/ database 路径之一有误；详见日志）";
        }
        result.models.push_back(m);
    }

    // 识别器初始化失败 => ok=false 并写入用户可见 warning。
    // ⚠️ 如实口径：这只是**启动期可见告警，并非硬阻断**——调用方 FramePipeline::initialize
    //    仅把该 warning 落到 render_.status，其 main() 也不检查返回值，服务仍照常启动；是否改为
    //    硬中止（fail-closed）属产品决策，不在本层处理。此处仍构造 DNN，供调用方决定是否降级。
    if (!recognizerOk) {
        result.ok = false;
        if (!result.warning.empty()) result.warning += "; ";
        // 归因如实：真实原因可能是 cascade 文件、ArcFace 识别模型 / 节点名或 database 路径，
        // 本层不区分，故文案不再指向任一具体配置项（详见识别器打印的日志）。
        result.warning += "识别模块初始化失败（cascade 文件 / 识别模型（含节点名）/ database 路径之一有误；详见日志）";
    } else {
        result.ok = true;
    }

    // --- 构造 DNN 检测器 ---
    result.detector = std::make_unique<DnnSsdFaceDetector>();
    if (cfg.dnn.enable) {
        DnnSsdConfig dc;
        dc.modelPath = cfg.dnn.modelPath;
        dc.configPath = cfg.dnn.configPath;
        dc.inputWidth = cfg.dnn.inputWidth;
        dc.inputHeight = cfg.dnn.inputHeight;
        dc.scale = cfg.dnn.scale;
        dc.meanB = cfg.dnn.meanB;
        dc.meanG = cfg.dnn.meanG;
        dc.meanR = cfg.dnn.meanR;
        dc.swapRB = cfg.dnn.swapRB;
        dc.confThreshold = cfg.dnn.confThreshold;
        dc.backend = cfg.dnn.backend;
        dc.target = cfg.dnn.target;

        std::string err;
        bool dnnOk = result.detector->initialize(dc, err);

        // DNN ModelSnapshot（无论初始化成功与否都记录）
        {
            ModelSnapshot m;
            m.id = "dnn_face_detector";
            m.displayName = "OpenCV DNN Face Detector";
            m.taskType = "detect";
            m.configuredPath = cfg.dnn.modelPath.string();
            m.resolvedPath = cfg.dnn.modelPath.string();
            m.backend = "opencv_dnn";
            m.hash = modelSha256OrUnavailable(cfg.dnn.modelPath);
            m.status = dnnOk ? "loaded" : (std::filesystem::exists(cfg.dnn.modelPath) ? "failed" : "missing");
            m.isInUse = true;
            m.lastError = err;
            result.models.push_back(m);
        }

        if (!dnnOk) {
            if (!result.warning.empty()) result.warning += "; ";
            result.warning += "DNN 检测初始化失败: " + err;
            // DNN 失败不阻断整体（ok 状态由识别器初始化成败决定）
        }
    } else {
        // DNN 禁用时仍记录快照，标记为 disabled
        ModelSnapshot m;
        m.id = "dnn_face_detector";
        m.displayName = "OpenCV DNN Face Detector";
        m.taskType = "detect";
        m.configuredPath = cfg.dnn.modelPath.string();
        m.resolvedPath = cfg.dnn.modelPath.string();
        m.backend = "opencv_dnn";
        m.hash = modelSha256OrUnavailable(cfg.dnn.modelPath);
        m.status = "disabled";
        m.isInUse = false;
        result.models.push_back(m);
    }

    return result;
}

}  // namespace rk_win
