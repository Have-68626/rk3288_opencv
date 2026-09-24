#include "pipeline/TrackCoordinator.h"
#include <algorithm>
#include <cmath>

namespace pipeline {

// 中心距离 + 尺寸比回退匹配（IoU 不足时的轻量跟踪，任务5）。
// 任务 C2：阈值由匿名 namespace 的编译期常量改为 TrackConfig 字段（默认值不变），
// 故本函数改为成员函数以读取 config_。原注释所载的取舍（过松→相邻脸串轨；过紧→大位移目标
// 每帧新建、永远达不到 stableFrames 而恒不显示）仍然成立，只是现在可注入以便调优与边界测试。
bool TrackCoordinator::centerSizeMatched(const cv::Rect& ref, const cv::Rect& det) const {
    const float cx1 = ref.x + ref.width / 2.0f;
    const float cy1 = ref.y + ref.height / 2.0f;
    const float cx2 = det.x + det.width / 2.0f;
    const float cy2 = det.y + det.height / 2.0f;
    const float dx = cx1 - cx2;
    const float dy = cy1 - cy2;
    const float dist = std::sqrt(dx * dx + dy * dy);
    const float refLen = static_cast<float>(std::max(ref.width, ref.height));
    if (refLen <= 0) return false;
    if (dist > config_.centerMaxRatio * refLen) return false;
    const float wr = static_cast<float>(det.width) / std::max(1, ref.width);
    const float hr = static_cast<float>(det.height) / std::max(1, ref.height);
    return wr >= config_.sizeRatioMin && wr <= config_.sizeRatioMax &&
           hr >= config_.sizeRatioMin && hr <= config_.sizeRatioMax;
}

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
    // ⚠️ 本早退位于 TTL 清理**之后是有意的，不是笔误**：TTL 是「长期无帧的最终保护」，即便本帧
    //    跳过检测也应照常回收；若把本早退上移到 TTL 之前，TTL 就会在节流期间失效。
    //    该顺序**已被用例双向钉住，不是「仅靠注释自觉」**：tests/cpp/test_track_coordinator.cpp 的
    //    TtlStillCullsTracksAsFallback 与 TtlBoundaryIsEnforced 均在 detectionRan=false 且超
    //    ttlMs 时断言轨迹被清理——把本早退上移到 TTL 之前即令二者翻红。
    //    真正**无覆盖**的是另一半：「生产侧恒传 detectionRan=true」。`detThrottleMode_` 的**全部**
    //    触碰点共三类，没有一类参与「本帧是否跳过检测」：① **store**——Engine::updateInferenceThrottle
    //    与 Engine::updateDetectionThrottle 各写一次；② **日志取值**——updateDetectionThrottle 内为拼
    //    `formatSelfCheckLine` 做的一次 load；③ **getter 的 load**——Engine::getDetectionThrottleMode。
    //    （原注释只举了 ①③ 两类，漏了 ② 这类日志取值，本批补齐。）故 `detThrottleMode_` **未接线**
    //    去跳过检测（Engine::trackFaces 处有同口径说明）⇒ **本早退分支在生产不可达**——不可达的是
    //    这条分支，而不是上面那个顺序；false 仅测试可构造。
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

#if defined(RK_ENABLE_TRACK_RESET_SEAM)
void TrackCoordinator::reset() {
    // 清空内部轨迹：撤销全部稳定身份（stableId），使新帧必须重新累计确认帧数，
    // 被删除的人不会再因旧轨迹残留 stableId 而持续输出 VERIFIED（第三波审查 #2）。
    // 刻意**不重置 nextTrackId_**：新轨迹使用全新 trackId，避免与已发布过的 ID 复用，
    // 消除「同一 trackId 先后指向不同目标」的对外歧义。
    tracks_.clear();
}
#endif

void TrackCoordinator::resetIdentitiesExcept(const std::vector<std::string>& keepIds) {
    // 第三波审查 #1：reset() 过宽——多人场景下删除/注册一人会瞬时抹掉其他人身份。
    // 本方法仅撤销「已不在本次图库有效集（keepIds）」中的稳定身份：把 stableId 清为
    // "Unknown" 并清零 stableConfidence，但**保留轨迹本体/bbox/确认计数**（不丢框、
    // 不需重新累计 stableFrames）。keepIds 中的身份原样保留。空 keepIds → 全部清为
    // "Unknown"（与 reset() 全清等价，但不丢框）。
    for (auto& t : tracks_) {
        // 未认证轨迹（"Unknown"）本就不持有有效身份，跳过（避免无谓重置 confidence）。
        if (t.view.stableId == "Unknown") continue;
        bool keep = false;
        for (const auto& id : keepIds) {
            if (t.view.stableId == id) { keep = true; break; }
        }
        if (!keep) {
            t.view.stableId = "Unknown";
            t.view.stableConfidence = 0.0f;
        }
    }
}

std::vector<TrackView> TrackCoordinator::views() const {
    std::vector<TrackView> out;
    out.reserve(tracks_.size());
    // 任务5：仅对外输出已确认轨迹（连续实际检测命中 ≥ stableFrames）。
    // 未确认轨迹仍保留在 tracks_ 内以累计确认计数、并在下次实际检测缺失时删除，
    // 但不进入对外视图——避免首帧误检立即显示（stableFrames 从删除规则升级为输出门槛）。
    for (const auto& t : tracks_) {
        if (t.confirmFrames >= config_.stableFrames) out.push_back(t.view);
    }
    return out;
}

}  // namespace pipeline