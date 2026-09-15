#pragma once
#include <array>
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

    // #1 注册采集：5 点关键点（x0,y0,...,x4,y4，统一变换后帧像素坐标）。
    // hasKeypoints=false 时 keypoints 无效。跟踪匹配不读取该字段，仅在注册采集的
    // 人脸对齐（关键点优先）与姿态分类时使用。刻意用纯 float 数组而非 cv::Point2f，
    // 使本头文件仍可被 RK_SKIP_OPENCV 的 core_unit_tests（mock OpenCV）编译。
    bool hasKeypoints = false;
    std::array<float, 10> keypoints{};
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

    // 纯函数：输入检测结果 + 时间戳 + 本次是否实际执行检测，输出跟踪视图
    // 任务5：确认与消失规则——实际检测(detectionRan=true)下，
    //   未确认轨迹（连续命中 < stableFrames）下一次实际检测缺失即删；
    //   已确认轨迹连续缺失 ≥2 次删除；命中帧递增确认计数。
    //   调度跳过检测(detectionRan=false)：不计缺失、不新建/匹配，保持现状。
    // 输出：返回值（views()）仅包含已确认轨迹（连续实际检测命中 ≥ stableFrames），
    //   未确认轨迹不对外输出（避免首帧误检立即显示）；内部仍保留以累计确认/删除计数。
    // 匹配：优先 IoU（matchIouThreshold），不足时允许中心距离+尺寸比回退。
    std::vector<TrackView> update(
        const std::vector<DetectedFace>& faces,
        long long timestampMs,
        bool detectionRan);

    // 清空全部内部轨迹（撤销所有稳定身份）。作为 TrackCoordinator 公共原语保留，
    // 供需要「硬清空」（连同轨迹本体与 bbox 一并丢弃）的场景或单测直接调用：
    // 被删除的人不应因旧轨迹仍持有其 stableId 而持续被判为 VERIFIED（第三波审查 #2）。
    // 注：Engine 侧图库变更统一走 applyGalleryUpdate→resetIdentitiesExcept（精准撤销、
    // 不丢框），不再经本全清路径。
    // **保留 nextTrackId_**：不回退计数器，避免新轨迹复用已对外发布过的 trackId，
    // 造成旧帧回调/日志中的 trackId 指向不同目标的复用歧义。
    // 线程安全：本方法非线程安全；调用方（Engine）须保证与 update() 互斥
    //（见 Engine::trackFaces 持 tracksMu_ 调用 update/reset）。
    void reset();

    // 按图库有效身份**精准撤销**（第三波审查 #1：reset() 过宽——多人场景删除/注册
    // **一人**会瞬时抹掉其他人的稳定身份、需重新累计 stableFrames）。
    // 语义：对 stableId **不在** keepIds（本次图库仍有效的身份集合）中的轨迹，仅把身份
    // 清为 "Unknown" 并清零 stableConfidence；**保留轨迹本体、bbox 与确认/缺失计数**，
    // 避免丢框、避免重新累计 stableFrames。keepIds 中的稳定身份原样保留。
    // 空 keepIds（删除末位人员 → 空图库）→ 全部清为 "Unknown"，与 reset() 的全清语义
    // 等价，但**不丢框**（reset() 会清空轨迹、需重新累计确认）。
    // 线程安全：与 reset() 一致，非线程安全；调用方（Engine）须持 tracksMu_ 与 update() 互斥。
    void resetIdentitiesExcept(const std::vector<std::string>& keepIds);

    int trackCount() const { return static_cast<int>(tracks_.size()); }

private:
    static float iou(const cv::Rect& a, const cv::Rect& b);
    std::vector<TrackView> views() const;

    // 内部轨迹状态：对外 TrackView + 确认/缺失计数（任务5 确认与消失规则）
    struct InternalTrack {
        TrackView view;
        int confirmFrames = 0;  // 连续实际检测命中帧数
        int missedActual = 0;   // 连续实际检测缺失帧数
    };

    TrackConfig config_;
    std::vector<InternalTrack> tracks_;
    int nextTrackId_ = 1;
};

}  // namespace pipeline
