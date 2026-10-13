#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <opencv2/core/types.hpp>  // cv::Rect

namespace pipeline {

struct DetectedFace {
    cv::Rect bbox;
    std::string identityId;
    float confidence = 0.0f;
    bool isAuthenticated = false;
};

struct TrackView {
    int trackId = 0;
    cv::Rect bbox;
    std::string stableId;          // 稳定后的身份 ID，"Unknown" 表示未认证
    float stableConfidence = 0.0f;
    long long lastSeenMs = 0;
};

struct TrackConfig {
    long long ttlMs = 1200;
    float matchIouThreshold = 0.3f;
    int stableFrames = 3;
};

class TrackCoordinator {
public:
    explicit TrackCoordinator(const TrackConfig& cfg = TrackConfig{});

    // 纯函数：输入检测结果 + 时间戳，输出跟踪视图
    // 不修改全局状态，不依赖完整 OpenCV（只有 cv::Rect）
    std::vector<TrackView> update(
        const std::vector<DetectedFace>& faces,
        long long timestampMs);

    int trackCount() const { return static_cast<int>(tracks_.size()); }

private:
    static float iou(const cv::Rect& a, const cv::Rect& b);

    TrackConfig config_;
    std::vector<TrackView> tracks_;
    int nextTrackId_ = 1;
};

}  // namespace pipeline
