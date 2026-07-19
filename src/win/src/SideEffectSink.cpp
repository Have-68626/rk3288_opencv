#include "rk_win/SideEffectSink.h"
#include "rk_win/FramePipeline.h"      // RenderState 完整定义
#include "rk_win/StructuredLogger.h"   // StructuredLogger, FrameLogEntry
#include "rk_win/FaceRecognizer.h"     // FaceMatch（用于渲染态转换）

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace rk_win {

// ──────────────────────────────────────────────
// 内部辅助：DetectMatch → FaceMatch
// ──────────────────────────────────────────────
static FaceMatch toFaceMatch(const DetectMatch& dm) {
    FaceMatch fm;
    fm.rect = dm.bbox;
    fm.personId = dm.personId;
    fm.distance = 0.0;
    fm.confidence = dm.confidence;
    fm.accepted = dm.isIdentified;
    return fm;
}

// ──────────────────────────────────────────────
// 内部辅助：DetectMatch → FaceLogEntry
// ──────────────────────────────────────────────
static FaceLogEntry toFaceLogEntry(const DetectMatch& dm) {
    FaceLogEntry fe;
    fe.x = dm.bbox.x;
    fe.y = dm.bbox.y;
    fe.w = dm.bbox.width;
    fe.h = dm.bbox.height;
    fe.personId = dm.personId;
    fe.distance = 0.0;
    fe.confidence = dm.confidence;
    return fe;
}

// ──────────────────────────────────────────────
// 辅助：置信度 0~1 → "xx%" 文本
// ──────────────────────────────────────────────
static std::string confidencePctText(double conf) {
    if (!std::isfinite(conf)) conf = 0.0;
    conf = std::clamp(conf, 0.0, 1.0);
    int pct = static_cast<int>(std::round(conf * 100.0));
    return std::to_string(pct) + "%";
}

// ──────────────────────────────────────────────
// 构造函数
// ──────────────────────────────────────────────
SideEffectSink::SideEffectSink(StructuredLogger* logger, RenderState* render)
    : logger_(logger), render_(render) {}

// ──────────────────────────────────────────────
// publish()：副作用统一收口
// 1. 在 drawFrame 上绘制 Overlay（矩形 + 人员标签）
// 2. 更新 render_ 的 bgr / faces（调用方需在外层持有 renderMu_ 锁）
// 3. 写结构化日志（若 logger_ 已打开）
// 4. 通知 facesCb_（若已设置）
// ──────────────────────────────────────────────
void SideEffectSink::publish(FrameResult& result, FrameLogEntry& logEntry) {
    // ── 1. Overlay 绘制 ──
    drawFacesOverlay(result.drawFrame, result.matches);

    // 在 move 之前保存帧尺寸（供日志使用）
    const int frameCols = result.drawFrame.cols;
    const int frameRows = result.drawFrame.rows;

    // ── 2. 更新渲染态（move 替代 copy） ──
    if (render_) {
        render_->bgr = std::move(result.drawFrame);
        render_->faces.clear();
        render_->faces.reserve(result.matches.size());
        for (const auto& dm : result.matches) {
            render_->faces.push_back(toFaceMatch(dm));
        }
    }

    // ── 3. 结构化日志 ──
    if (logger_) {
        // 补充 faces 字段
        logEntry.faces.clear();
        logEntry.faces.reserve(result.matches.size());
        for (const auto& dm : result.matches) {
            logEntry.faces.push_back(toFaceLogEntry(dm));
        }

        // 补充帧尺寸（move 前已保存）
        logEntry.frameWidth = frameCols;
        logEntry.frameHeight = frameRows;

        logger_->append(logEntry);
    }

    // ── 4. 回调通知（如 WebSocket 广播） ──
    if (facesCb_) {
        facesCb_(result.matches);
    }
}

// ──────────────────────────────────────────────
// drawFacesOverlay()：在帧上绘制面框 + 人员标签
// - 已识别人员（isIdentified=true）：绿色边框 + ID + 置信度
// - 未识别人员（isIdentified=false）：红色边框 + 置信度
// ──────────────────────────────────────────────
void SideEffectSink::drawFacesOverlay(cv::Mat& draw, const std::vector<DetectMatch>& matches) {
    if (draw.empty() || matches.empty()) return;

    for (const auto& m : matches) {
        // 已识别 → 绿色 (BGR), 未识别 → 红色 (BGR)
        const cv::Scalar color = m.isIdentified
                                     ? cv::Scalar(0, 200, 0)    // 绿色
                                     : cv::Scalar(0, 0, 200);   // 红色

        constexpr int thickness = 2;
        cv::rectangle(draw, m.bbox, color, thickness);

        // 标签文本：已识别 → "personId 90%", 未识别 → "90%"
        std::string label;
        if (m.isIdentified) {
            label = m.personId + " " + confidencePctText(m.confidence);
        } else {
            label = confidencePctText(m.confidence);
        }

        // 标签位置：框左上角上方
        const int labelY = std::max(0, m.bbox.y - 6);
        const int labelX = std::max(0, m.bbox.x + 2);
        cv::putText(draw, label, cv::Point(labelX, labelY),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1, cv::LINE_AA);
    }
}

}  // namespace rk_win
