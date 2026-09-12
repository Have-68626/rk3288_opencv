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
