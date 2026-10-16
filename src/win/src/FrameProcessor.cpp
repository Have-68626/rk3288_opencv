#include "rk_win/FrameProcessor.h"
#include "rk_win/IRecognizer.h"
#include "rk_win/FaceRecognizer.h"   // FaceMatch 完整定义（IRecognizer 仅前向声明）

#include <chrono>

namespace rk_win {

// ──────────────────────────────────────────────
// 内部辅助：将 FaceMatch → DetectMatch
// ──────────────────────────────────────────────
static DetectMatch fromFaceMatch(const FaceMatch& fm) {
    DetectMatch m;
    m.bbox = fm.rect;
    m.confidence = static_cast<float>(fm.confidence);
    m.personId = fm.personId;
    // 拒识沿用非空的 UNKNOWN 标签，身份判定必须消费识别器的接受结果。
    m.isIdentified = fm.accepted;
    return m;
}

// ──────────────────────────────────────────────
// 构造函数：持非拥有指针，调用方保证生命周期
// ──────────────────────────────────────────────
FrameProcessor::FrameProcessor(IRecognizer* recognizer)
    : recognizer_(recognizer) {}

// ──────────────────────────────────────────────
// run()：纯计算入口
// 1. 每帧识别，实际 stride 固定为 1
// 2. 清库指令消费
// 3. 注册指令消费
// 4. 识别器 identify（包含检测），计量实际耗时
// 5. 移动帧 → 组装 FrameResult 返回
// ──────────────────────────────────────────────
FrameResult FrameProcessor::run(cv::Mat bgr, ControlCommand& cmd) {
    FrameResult result;

    // 当前识别器每帧独立检测与识别，不能把未生效的跳帧配置上报为实际 stride。
    cmd.frameCounter++;
    cmd.detectStride = 1;

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

    // identify 已返回完整检测与识别结果，额外 SSD 前向没有消费者，不能计入识别耗时。
    const auto inferStart = std::chrono::steady_clock::now();
    std::vector<FaceMatch> faceMatches = recognizer_->identify(bgr);
    lastInferMs_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - inferStart).count();

    std::vector<DetectMatch> matches;
    matches.reserve(faceMatches.size());
    for (const auto& fm : faceMatches) {
        matches.push_back(fromFaceMatch(fm));
    }

    // ── 5. 移动帧 + 组装结果 ──
    result.drawFrame = std::move(bgr);
    result.matches = std::move(matches);
    result.consumedCommand = cmd;
    result.hasMatch = !result.matches.empty();
    result.inferMs = lastInferMs_;

    return result;
}

}  // namespace rk_win
