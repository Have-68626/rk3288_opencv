#include "pipeline/TrackCoordinator.h"
#include <algorithm>
#include <cmath>

namespace pipeline {

namespace {

// 中心距离 + 尺寸比 fallback（IoU 不足时的轻量跟踪，任务5）
constexpr float kCenterMaxRatio = 0.8f;  // 中心偏移 / max(宽,高) 上限
constexpr float kSizeRatioMin = 0.5f;
constexpr float kSizeRatioMax = 2.0f;

// 中心距离与尺寸比是否匹配（仅在 IoU 阈值不足时使用，避免相邻脸误合并）
bool centerSizeMatched(const cv::Rect& ref, const cv::Rect& det) {
    const float cx1 = ref.x + ref.width / 2.0f;
    const float cy1 = ref.y + ref.height / 2.0f;
    const float cx2 = det.x + det.width / 2.0f;
    const float cy2 = det.y + det.height / 2.0f;
    const float dx = cx1 - cx2;
    const float dy = cy1 - cy2;
    const float dist = std::sqrt(dx * dx + dy * dy);
    const float refLen = static_cast<float>(std::max(ref.width, ref.height));
    if (refLen <= 0) return false;
    if (dist > kCenterMaxRatio * refLen) return false;
    const float wr = static_cast<float>(det.width) / std::max(1, ref.width);
    const float hr = static_cast<float>(det.height) / std::max(1, ref.height);
    return wr >= kSizeRatioMin && wr <= kSizeRatioMax &&
           hr >= kSizeRatioMin && hr <= kSizeRatioMax;
}

}  // namespace

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
    long long timestampMs,
    bool detectionRan)
{
    // 0. TTL 兜底清理（主动删除由下方缺失计数承担，TTL 仅作长期无帧的最终保护）
    std::vector<InternalTrack> alive;
    alive.reserve(tracks_.size());
    for (auto& t : tracks_) {
        if (timestampMs - t.view.lastSeenMs <= config_.ttlMs) alive.push_back(t);
    }
    tracks_ = std::move(alive);

    // 性能调度跳过检测：不计缺失、不新建/匹配（任务5：保持现状）
    if (!detectionRan) {
        return views();
    }

    // 1. 一对一匹配：优先 IoU，不足时中心距离+尺寸比回退
    std::vector<bool> trackUsed(tracks_.size(), false);
    std::vector<int> detToTrack(faces.size(), -1);

    // 按面积降序确保确定性
    std::vector<int> order(faces.size());
    for (int i = 0; i < static_cast<int>(faces.size()); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return faces[a].bbox.area() > faces[b].bbox.area();
    });

    for (int idx : order) {
        int bestTrack = -1;
        float bestIou = 0.0f;
        for (int ti = 0; ti < static_cast<int>(tracks_.size()); ti++) {
            if (trackUsed[ti]) continue;
            float s = iou(tracks_[ti].view.bbox, faces[idx].bbox);
            if (s > bestIou) { bestIou = s; bestTrack = ti; }
        }
        if (bestTrack >= 0 && bestIou >= config_.matchIouThreshold) {
            trackUsed[bestTrack] = true;
            detToTrack[idx] = bestTrack;
            continue;
        }
        // IoU 不足 → 中心距离+尺寸比回退（不抢占已 IoU 匹配的 track）
        for (int ti = 0; ti < static_cast<int>(tracks_.size()); ti++) {
            if (trackUsed[ti]) continue;
            if (centerSizeMatched(tracks_[ti].view.bbox, faces[idx].bbox)) {
                trackUsed[ti] = true;
                detToTrack[idx] = ti;
                break;
            }
        }
    }

    // 2. 更新已有 track，维护确认/缺失计数
    std::vector<InternalTrack> after;
    after.reserve(tracks_.size() + faces.size());
    for (int ti = 0; ti < static_cast<int>(tracks_.size()); ti++) {
        int matchedDet = -1;
        for (int i = 0; i < static_cast<int>(faces.size()); i++) {
            if (detToTrack[i] == ti) { matchedDet = i; break; }
        }
        if (matchedDet >= 0) {
            InternalTrack t = tracks_[ti];
            t.confirmFrames = std::min(t.confirmFrames + 1, 1000000);
            t.missedActual = 0;
            t.view.bbox = faces[matchedDet].bbox;
            t.view.lastSeenMs = timestampMs;
            const auto& df = faces[matchedDet];
            if (df.isAuthenticated) {
                if (t.view.stableId == "Unknown") {
                    t.view.stableId = df.identityId;
                    t.view.stableConfidence = df.confidence;
                }
                // 不同身份不覆盖
            }
            // 非认证检测不覆盖身份
            after.push_back(t);
        } else {
            // 实际检测运行但本 track 无命中
            InternalTrack t = tracks_[ti];
            t.missedActual += 1;
            const bool confirmed = t.confirmFrames >= config_.stableFrames;
            if (!confirmed) continue;          // 未确认：下一次实际检测缺失即删
            if (t.missedActual >= 2) continue; // 已确认：连续缺失 2 次删除
            after.push_back(t);
        }
    }

    // 3. 未匹配的检测 → 新建 track（首次命中即计一次确认帧）
    for (int i = 0; i < static_cast<int>(faces.size()); i++) {
        if (detToTrack[i] >= 0) continue;
        InternalTrack t;
        t.view.trackId = nextTrackId_++;
        t.view.bbox = faces[i].bbox;
        t.view.stableId = faces[i].isAuthenticated ? faces[i].identityId : "Unknown";
        t.view.stableConfidence = faces[i].confidence;
        t.view.lastSeenMs = timestampMs;
        t.confirmFrames = 1;  // 首帧即首次实际检测命中
        after.push_back(t);
    }

    tracks_ = std::move(after);
    return views();
}

std::vector<TrackView> TrackCoordinator::views() const {
    std::vector<TrackView> out;
    out.reserve(tracks_.size());
    for (const auto& t : tracks_) out.push_back(t.view);
    return out;
}

}  // namespace pipeline