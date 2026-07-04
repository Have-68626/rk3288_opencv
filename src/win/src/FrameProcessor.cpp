#include "rk_win/FrameProcessor.h"
#include "rk_win/DnnSsdFaceDetector.h"
#include "rk_win/IRecognizer.h"
#include "rk_win/FaceRecognizer.h"   // FaceMatch 完整定义（IRecognizer 仅前向声明）

#include <algorithm>

namespace rk_win {

// ──────────────────────────────────────────────
// 内部辅助：将 DnnFaceDetection → DetectMatch
// ──────────────────────────────────────────────
static DetectMatch fromDnnDetection(const DnnFaceDetection& d) {
    DetectMatch m;
    m.bbox = d.rect;
    m.confidence = static_cast<float>(d.confidence);
    m.personId.clear();
    m.isIdentified = false;
    return m;
}

// ──────────────────────────────────────────────
// 内部辅助：将 FaceMatch → DetectMatch
// ──────────────────────────────────────────────
static DetectMatch fromFaceMatch(const FaceMatch& fm) {
    DetectMatch m;
    m.bbox = fm.rect;
    m.confidence = static_cast<float>(fm.confidence);
    m.personId = fm.personId;
    m.isIdentified = !fm.personId.empty();
    return m;
}

// ──────────────────────────────────────────────
// 构造函数：持非拥有指针，调用方保证生命周期
// ──────────────────────────────────────────────
FrameProcessor::FrameProcessor(DnnSsdFaceDetector* dnn, IRecognizer* recognizer)
    : dnn_(dnn), recognizer_(recognizer) {}

// ──────────────────────────────────────────────
// run()：纯计算入口
// 1. 帧计数器自增 + 背压跳过判断
// 2. 清库指令消费
// 3. 注册指令消费
// 4. DNN 检测（可选，跳过时复用缓存）
// 5. 识别器 identify（总是执行，覆盖 DNN 输出）
// 6. 复制帧副本 → 组装 FrameResult 返回
// ──────────────────────────────────────────────
FrameResult FrameProcessor::run(const cv::Mat& bgr, ControlCommand& cmd) {
    FrameResult result;

    // ── 1. 帧计数器 + 背压跳过判断 ──
    cmd.frameCounter++;
    bool doInfer = true;
    if (dnn_ && dnn_->ready()) {
        doInfer = (cmd.detectStride <= 1)
                      ? true
                      : ((cmd.frameCounter % cmd.detectStride) == 0 || lastDetections_.empty());
    }

    // ── 2. 清库 ──
    if (cmd.clearDb) {
        recognizer_->clearDb();
        cmd.clearDb = false;  // 已消费
    }

    // ── 3. 注册 ──
    if (cmd.enrollRequested && cmd.enrollRemaining > 0) {
        int taken = 0;
        if (recognizer_->enrollFromFrame(cmd.enrollPersonId, bgr, 1, taken) && taken > 0) {
            cmd.enrollRemaining -= taken;
            recognizer_->saveDb();
        }
        if (cmd.enrollRemaining <= 0) {
            cmd.enrollRequested = false;
        }
    }

    // ── 4. DNN 检测 ──
    // 使用 DNN 做粗略检测定位（测量耗时用于 stride 自适应）
    if (dnn_ && dnn_->ready() && doInfer) {
        double latencyMs = 0.0;
        std::vector<DnnFaceDetection> dets = dnn_->detect(bgr, latencyMs);
        lastInferMs_ = latencyMs;

        lastDetections_.clear();
        lastDetections_.reserve(dets.size());
        for (const auto& d : dets) {
            lastDetections_.push_back(fromDnnDetection(d));
        }
    }

    // ── 5. 识别器 identify（总是执行，覆盖 DNN 输出）──
    // ⚠️ 注意：当前逻辑中 recognizer_->identify 返回完整检测+识别结果，
    //    会覆盖 DNN 检测阶段的 matches。DNN 输出保存在 lastDetections_
    //    供背压跳帧时复用，但即使跳帧时 identify 仍每帧执行。
    std::vector<FaceMatch> faceMatches = recognizer_->identify(bgr);

    std::vector<DetectMatch> matches;
    matches.reserve(faceMatches.size());
    for (const auto& fm : faceMatches) {
        matches.push_back(fromFaceMatch(fm));
    }

    // ── 6. 复制帧副本 + 组装结果 ──
    bgr.copyTo(result.drawFrame);
    result.matches = std::move(matches);
    result.consumedCommand = cmd;
    result.hasMatch = !result.matches.empty();
    result.inferMs = lastInferMs_;

    return result;
}

}  // namespace rk_win
