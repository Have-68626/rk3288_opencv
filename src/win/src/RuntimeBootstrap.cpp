#include "rk_win/RuntimeBootstrap.h"
#include "rk_win/IRecognizer.h"
#include "rk_win/FaceRecognizer.h"
#include "rk_win/ArcFaceWinRecognizer.h"
#include "rk_win/DnnSsdFaceDetector.h"
#include "rk_win/WinConfig.h"
#include "FileHash.h"

#include <filesystem>

namespace rk_win {

BootstrapResult::~BootstrapResult() = default;

BootstrapResult RuntimeBootstrap::build(const AppConfig& cfg) {
    BootstrapResult result;

    const std::string cascadeUtf8 = cfg.recognition.cascadePath.string();

    // --- 构造识别器（ArcFace 或 LBPH）---
    bool cascadeOk = false;
    if (cfg.model.recognition == "arcface") {
        auto arcRec = std::make_unique<ArcFaceWinRecognizer>();
        cascadeOk = arcRec->initialize(
            cascadeUtf8,
            cfg.recognition.databasePath,
            cfg.recognition.arcFaceModelPath.string(),
            cfg.recognition.minFaceSizePx,
            cfg.recognition.identifyThreshold);
        result.recognizer = std::move(arcRec);
    } else {
        auto lbpRec = std::make_unique<FaceRecognizer>();
        cascadeOk = lbpRec->initialize(
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
        if (cfg.model.recognition == "arcface") {
            m.hash = rk_wcfr::calculateSHA256(cfg.recognition.arcFaceModelPath.string());
        } else {
            m.hash = rk_wcfr::calculateSHA256(cfg.recognition.cascadePath);
        }
        m.status = cascadeOk ? "loaded" : "failed";
        m.isInUse = !cfg.dnn.enable;
        if (!cascadeOk) {
            m.lastError = "初始化失败 (cascade_path 或 database_path 有误)";
        }
        result.models.push_back(m);
    }

    // 关键组件 cascade 失败 => 整体失败（但仍构造 DNN，供调用方决定是否降级）
    if (!cascadeOk) {
        result.ok = false;
        if (!result.warning.empty()) result.warning += "; ";
        result.warning += "识别模块初始化失败（请检查 cascade_path 与 database_path）";
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
            m.hash = rk_wcfr::calculateSHA256(cfg.dnn.modelPath);
            m.status = dnnOk ? "loaded" : (std::filesystem::exists(cfg.dnn.modelPath) ? "failed" : "missing");
            m.isInUse = true;
            m.lastError = err;
            result.models.push_back(m);
        }

        if (!dnnOk) {
            if (!result.warning.empty()) result.warning += "; ";
            result.warning += "DNN 检测初始化失败: " + err;
            // DNN 失败不阻断整体（ok 状态由 cascade 决定）
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
        m.hash = rk_wcfr::calculateSHA256(cfg.dnn.modelPath);
        m.status = "disabled";
        m.isInUse = false;
        result.models.push_back(m);
    }

    return result;
}

}  // namespace rk_win
