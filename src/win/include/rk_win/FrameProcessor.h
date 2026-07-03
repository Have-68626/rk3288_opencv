#pragma once
#include <opencv2/core.hpp>
#include <vector>
#include <string>

namespace rk_win {

class DnnSsdFaceDetector;
class IRecognizer;

// 检测+识别结果，替代原 FaceMatch 用于解耦后热路径数据传递
struct DetectMatch {
    cv::Rect bbox;
    float confidence = 0.0f;
    std::string personId;
    bool isIdentified = false;  // true 表示已识别出特定人员
};

// 控制命令：调用方每一帧传入以驱动清库、注册、背压行为
struct ControlCommand {
    bool clearDb = false;
    bool enrollRequested = false;
    std::string enrollPersonId;
    int enrollRemaining = 0;
    int detectStride = 1;
    int frameCounter = 0;
};

// FrameProcessor::run() 的输出：绘制帧、匹配结果、消耗后的命令状态
struct FrameResult {
    cv::Mat drawFrame;                        // 帧副本（供后续 Overlay 绘制）
    std::vector<DetectMatch> matches;          // 检测+识别结果
    ControlCommand consumedCommand;            // 命令消耗后的状态（调用方用于同步）
    bool hasMatch = false;
    double inferMs = 0.0;                      // 本次推理耗时（用于 stride 自适应）
};

// 纯计算处理器：取帧 → 推理 → 返回结果，不写日志不更新 UI
class FrameProcessor {
public:
    FrameProcessor(DnnSsdFaceDetector* dnn, IRecognizer* recognizer);

    // 核心方法：对输入帧 bgr 执行清库/注册/检测/识别，返回 FrameResult
    FrameResult run(const cv::Mat& bgr, ControlCommand& cmd);

    // 最近一次推理耗时（供调用方 stride 自适应参考）
    double lastInferMs() const { return lastInferMs_; }

private:
    DnnSsdFaceDetector* dnn_;
    IRecognizer* recognizer_;

    std::vector<DetectMatch> lastDetections_;  // DNN 检测缓存（背压跳帧时复用）
    double lastInferMs_ = 0.0;
};

}  // namespace rk_win
