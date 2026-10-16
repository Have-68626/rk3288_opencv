#pragma once
#include <cstdint>
#include <opencv2/core.hpp>
#include <vector>
#include <string>

namespace rk_win {

class IRecognizer;

// 检测+识别结果，替代原 FaceMatch 用于解耦后热路径数据传递
struct DetectMatch {
    cv::Rect bbox;
    float confidence = 0.0f;
    std::string personId;
    bool isIdentified = false;  // true 表示已识别出特定人员
};

// 控制命令：调用方每一帧传入以驱动清库、注册行为
struct ControlCommand {
    bool clearDb = false;
    bool enrollRequested = false;
    std::string enrollPersonId;
    int enrollRemaining = 0;
    std::uint64_t enrollEpoch = 0;  // 注册代次：requestEnroll 递增，随命令透传；FramePipeline 回写时凭它守卫不覆盖新请求
    int detectStride = 1;
    int frameCounter = 0;
};

// FrameProcessor::run() 的输出：绘制帧、匹配结果、消耗后的命令状态
struct FrameResult {
    cv::Mat drawFrame;                        // 帧副本（供后续 Overlay 绘制）
    std::vector<DetectMatch> matches;          // 检测+识别结果
    ControlCommand consumedCommand;            // 命令消耗后的状态（调用方用于同步）
    bool hasMatch = false;
    double inferMs = 0.0;                      // 本帧 identify 的检测与识别耗时
};

// 纯计算处理器：取帧 → 推理 → 返回结果，不写日志不更新 UI
class FrameProcessor {
public:
    explicit FrameProcessor(IRecognizer* recognizer);

    // 核心方法：对输入帧 bgr 执行清库/注册/检测/识别，返回 FrameResult
    FrameResult run(cv::Mat bgr, ControlCommand& cmd);

    // 最近一帧 identify 的检测与识别耗时
    double lastInferMs() const { return lastInferMs_; }

private:
    IRecognizer* recognizer_;
    double lastInferMs_ = 0.0;
};

}  // namespace rk_win
