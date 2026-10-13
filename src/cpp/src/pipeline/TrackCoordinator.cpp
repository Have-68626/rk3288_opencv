#include "pipeline/TrackCoordinator.h"
#include <algorithm>

namespace pipeline {

TrackCoordinator::TrackCoordinator(const TrackConfig& cfg) : config_(cfg) {}

float TrackCoordinator::iou(const cv::Rect& a, const cv::Rect& b) {
    int x1 = std::max(a.x, b.x);
    int y1 = std::max(a.y, b.y);
    int x2 = std::min(a.x + a.width, b.x + b.width);
    int y2 = std::min(a.y + a.height, b.y + b.height);
    if (x2 <= x1 || y2 <= y1) return 0.0f;
    float inter = static_cast<float>((x2 - x1) * (y2 - y1));
    float uni = static_cast<float>(a.area() + b.area()) - inter;
    return uni > 0 ? inter / uni : 0.0f;
}

std::vector<TrackView> TrackCoordinator::update(
    const std::vector<DetectedFace>& faces,
    long long timestampMs)
{
    // 1. TTL 清理
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
        [&](const TrackView& t) {
            return timestampMs - t.lastSeenMs > config_.ttlMs;
        }), tracks_.end());

    if (faces.empty()) {
        return tracks_;
    }

    // 2. 按面积降序
    std::vector<int> order(faces.size());
    for (int i = 0; i < static_cast<int>(faces.size()); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return faces[a].bbox.area() > faces[b].bbox.area();
    });

    // 3. IoU 贪心匹配
    std::vector<bool> trackUsed(tracks_.size(), false);
    std::vector<int> detToTrack(faces.size(), -1);

    for (int idx : order) {
        float best = 0.0f;
        int bestTrack = -1;
        for (int ti = 0; ti < static_cast<int>(tracks_.size()); ti++) {
            if (trackUsed[ti]) continue;
            float s = iou(tracks_[ti].bbox, faces[idx].bbox);
            if (s > best) { best = s; bestTrack = ti; }
        }
        if (bestTrack >= 0 && best >= config_.matchIouThreshold) {
            trackUsed[bestTrack] = true;
            detToTrack[idx] = bestTrack;
        }
    }

    // 4. 未匹配的检测 -> 创建新 track
    for (int i = 0; i < static_cast<int>(faces.size()); i++) {
        if (detToTrack[i] >= 0) continue;
        TrackView t;
        t.trackId = nextTrackId_++;
        t.bbox = faces[i].bbox;
        t.stableId = faces[i].isAuthenticated ? faces[i].identityId : "Unknown";
        t.stableConfidence = faces[i].confidence;
        t.lastSeenMs = timestampMs;
        tracks_.push_back(t);
        detToTrack[i] = static_cast<int>(tracks_.size()) - 1;
    }

    // 5. 更新已有 track（已认证身份不覆盖）
    for (int i = 0; i < static_cast<int>(faces.size()); i++) {
        int ti = detToTrack[i];
        if (ti < 0) continue;
        auto& t = tracks_[ti];
        t.bbox = faces[i].bbox;
        t.lastSeenMs = timestampMs;

        if (faces[i].isAuthenticated) {
            if (faces[i].identityId == t.stableId) {
                // 同一身份，维持
            } else if (t.stableId == "Unknown") {
                // 首次认证
                t.stableId = faces[i].identityId;
                t.stableConfidence = faces[i].confidence;
            }
            // 不同身份不覆盖
        }
        // 非认证帧不覆盖
    }

    return tracks_;
}

}  // namespace pipeline
