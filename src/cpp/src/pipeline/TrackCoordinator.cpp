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

// A2 身份撤销：把稳定身份与候选一并清掉。用于「换人 / 未识别 / 识别失败 / 实际漏检」四类事件——
// 它们都意味着本轨迹当前掌握的认证结论不再成立，必须立即撤销而非「保持 Unknown 不动」，
// 否则候选身份会带着旧计数跨过这些事件继续累计，第 3 次时「确认」出一个已经中断过的结论。
void TrackCoordinator::clearIdentity(InternalTrack& t) {
    t.view.stableId = "Unknown";
    t.view.stableConfidence = 0.0f;
    t.candidateId.clear();
    t.candidateHits = 0;
}

// A2 三次实际识别：仅当**连续**观察到同一真实身份达到固定门槛（3 次）时才置 stableId。
// 未认证 / 空身份 / "Unknown" 一律撤销（不参与累计）；换人先撤销再以新身份重新从 1 计。
// 门槛刻意写死为 3（不受 TrackConfig::stableFrames 影响）——stableFrames 管的是**框**的确认。
void TrackCoordinator::observeIdentity(InternalTrack& t, const DetectedFace& face) {
    if (!face.isAuthenticated || face.identityId.empty() || face.identityId == "Unknown") {
        clearIdentity(t);
        return;
    }
    if (t.candidateId != face.identityId) {
        clearIdentity(t);
        t.candidateId = face.identityId;
        t.candidateHits = 1;
    } else {
        t.candidateHits = std::min(3, t.candidateHits + 1);
    }
    if (t.candidateHits == 3) {
        t.view.stableId = face.identityId;
        t.view.stableConfidence = face.confidence;
    } else {
        t.view.stableId = "Unknown";
        t.view.stableConfidence = 0.0f;
    }
}

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
            // A2：逐帧按实际识别结果更新身份候选（未识别/换人即撤销，连续 3 次同身份才 VERIFIED）。
            observeIdentity(t, faces[matchedDet]);
            after.push_back(t);
        } else {
            // 实际检测运行但本 track 无命中：实际漏检 ⇒ **立即撤销**身份与候选（A2），
            // 再走旧的框消失规则（未确认：下次缺失即删；已确认：连续 2 次缺失删除）。
            InternalTrack t = tracks_[ti];
            clearIdentity(t);
            t.missedActual += 1;
            const bool confirmed = t.confirmFrames >= config_.stableFrames;
            if (!confirmed) continue;          // 未确认：下一次实际检测缺失即删
            if (t.missedActual >= 2) continue; // 已确认：连续缺失 2 次删除
            after.push_back(t);
        }
    }

    // 3. 未匹配的检测 → 新建 track（首次命中即计一次框确认帧、一次身份识别）
    for (int i = 0; i < static_cast<int>(faces.size()); i++) {
        if (detToTrack[i] >= 0) continue;
        InternalTrack t;
        t.view.trackId = nextTrackId_++;
        t.view.bbox = faces[i].bbox;
        t.view.lastSeenMs = timestampMs;
        t.confirmFrames = 1;  // 首帧即首次实际检测命中
        // A2：首帧即第 1 次身份识别；未满 3 次前 stableId 保持 "Unknown"
        //（因此签名里的「首帧即透传身份」不再成立——身份须另行连续累计 3 次）。
        observeIdentity(t, faces[i]);
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
    // 本方法仅撤销「已不在本次图库有效集（keepIds）」中的身份：清为 "Unknown" 并清零
    // stableConfidence，但**保留轨迹本体/bbox/确认计数**（不丢框、不需重新累计 stableFrames）。
    // A2：候选身份（candidateId）是「正在累计、尚未确认」的另一半身份真相，同样按 keepIds 撤销——
    // 只按 stableId 保留会让被删除者的候选继续累计、在第 3 次「确认」出已不在图库的身份。
    auto kept = [&keepIds](const std::string& id) {
        if (id.empty() || id == "Unknown") return false;
        for (const auto& k : keepIds) {
            if (k == id) return true;
        }
        return false;
    };
    for (auto& t : tracks_) {
        if (t.view.stableId != "Unknown" && !kept(t.view.stableId)) {
            t.view.stableId = "Unknown";
            t.view.stableConfidence = 0.0f;
        }
        if (!t.candidateId.empty() && !kept(t.candidateId)) {
            t.candidateId.clear();
            t.candidateHits = 0;
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