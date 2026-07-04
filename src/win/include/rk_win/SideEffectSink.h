#pragma once
#include <opencv2/core.hpp>
#include <functional>
#include <vector>
#include <string>

#include "rk_win/FrameProcessor.h"

namespace rk_win {

// 前向声明（定义分别在 FramePipeline.h / StructuredLogger.h）
struct RenderState;
class StructuredLogger;
struct FrameLogEntry;

// 副作用收口：负责 Overlay 绘制、渲染态发布、结构化日志写入、回调通知
class SideEffectSink {
public:
    // logger 和 render 均可为 nullptr（单元测试时不传）
    SideEffectSink(StructuredLogger* logger, RenderState* render);

    // 发布副作用：Overlay 绘制 → 渲染态发布 → 结构化日志 → 回调通知
    // logEntry 由调用方填充元数据（时间戳、相机名等），本方法补充 faces 后写入 logger_
    void publish(FrameResult& result, FrameLogEntry& logEntry);

    // 可选回调：匹配结果通知调用方（如 WebSocket 广播）
    using FacesCallback = std::function<void(const std::vector<DetectMatch>&)>;
    void setFacesCallback(FacesCallback cb) { facesCb_ = std::move(cb); }

private:
    // 在 result.drawFrame 上绘制矩形 + 人员标签（已识别绿色，未识别红色）
    void drawFacesOverlay(cv::Mat& draw, const std::vector<DetectMatch>& matches);

    StructuredLogger* logger_;
    RenderState* render_;
    FacesCallback facesCb_;
};

}  // namespace rk_win
