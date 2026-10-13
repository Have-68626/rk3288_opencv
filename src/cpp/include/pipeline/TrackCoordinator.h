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

    // 中心距离回退匹配的三个阈值（任务 C2）。此前是 TrackCoordinator.cpp 匿名 namespace 里的
    // **编译期常量**——完全不可注入（无 CMake -D、无环境变量），阈值调优只能改源码重编。
    // 提升为配置项后默认值**保持原值**，Engine 仍用默认构造 → **零行为变更**；
    // 而「录像回放扫描阈值」与「边界用例」得以在不改源码的前提下进行。
    //
    // 取值依据（如实说明）：0.8/0.5/2.0 由 4e87023 引入，**尚无真实场景调优证据**。
    // 已实测的失效边界是「单帧位移 ≈ 框宽的 0.8 倍」——见 test_track_coordinator.cpp 的
    // ContinuouslyMovingTargetCompletesInitialConfirmation（79px/帧可持续确认）与
    // DisplacementBeyondFallbackRatioNeverConfirms（81px/帧永不确认）。
    //
    // 语义：中心偏移超过 centerMaxRatio × max(refW,refH) 即不匹配（过松会让相邻脸被串到同一轨迹，
    // 过紧则大位移目标每帧新建轨迹、永远达不到 stableFrames 而在 views() 中恒不显示）；
    // 尺寸比需落在 [sizeRatioMin, sizeRatioMax] 内。
    float centerMaxRatio = 0.8f;
    float sizeRatioMin = 0.5f;
    float sizeRatioMax = 2.0f;
};

class TrackCoordinator {
public:
    explicit TrackCoordinator(const TrackConfig& cfg = TrackConfig{});

    // 纯函数：输入检测结果 + 时间戳 + 本次是否实际执行检测，输出跟踪视图
    // 任务5：确认与消失规则——实际检测(detectionRan=true)下，
    //   未确认轨迹（连续命中 < stableFrames）下一次实际检测缺失即删；
    //   已确认轨迹连续缺失 ≥2 次删除；命中帧递增确认计数并**使缺失计数清零**
    //   （InternalTrack::missedActual = 0——已确认轨迹的「连续缺失」是连续语义，命中即断链）。
    //   调度跳过检测(detectionRan=false)：不计缺失、不新建/匹配，保持现状。
    //   ⚠️ 注意：TTL 清理在本分支之前无条件执行，故跳过检测时 TTL 仍照常生效（由两条 TTL
    //   用例钉住，见 TrackCoordinator.cpp 中的顺序说明）；而本分支在生产**不可达**——调用方
    //   Engine::trackFaces 恒传 detectionRan=true（detThrottleMode_ 未接线去跳过检测）。
    // 输出：返回值（views()）仅包含已确认轨迹（连续实际检测命中 ≥ stableFrames），
    //   未确认轨迹不对外输出（避免首帧误检立即显示）；内部仍保留以累计确认/删除计数。
    //
    // A2：身份确认与「框确认」**解耦**。身份采用「同身份连续 3 次实际识别才置为 VERIFIED」的
    //   固定门槛（与 TrackConfig::stableFrames 无关）：连续命中同一身份 3 次才写入 stableId；
    //   换人、未识别（未认证 / 空身份 / "Unknown"）、识别失败、以及**实际漏检** 均**立即撤销**
    //   已确认身份并把候选计数清零、重新累计——不再有「旧身份黏住、同位置换人永不改写」的行为。
    //   结果是：框（已确认，≥stableFrames）可以对外可见，但其 stableId 仍可能是 "Unknown"
    //   （身份尚在累计中）——「框可见」不等于「身份已 VERIFIED」。
    // 匹配：优先 IoU（matchIouThreshold），不足时允许中心距离+尺寸比回退。
    std::vector<TrackView> update(
        const std::vector<DetectedFace>& faces,
        long long timestampMs,
        bool detectionRan);

    // 测试接缝：清空全部内部轨迹（撤销所有稳定身份）。当前**生产侧零调用**——Engine 的图库
    // 变更一律走 resetIdentitiesExcept（精准撤销、不丢框），不经本全清路径；实际调用者只有
    // 单测与回放载体（test_track_coordinator / test_track_replay）。
    // 语义：新帧必须重新累计确认帧数，被删除的人不会因旧轨迹残留 stableId 而持续被判 VERIFIED
    //   （第三波审查 #2）；**保留 nextTrackId_**：不回退计数器，避免新轨迹复用已对外发布过的
    //   trackId，造成「同一 trackId 先后指向不同目标」的复用歧义。
    // 线程安全：非线程安全；调用方须保证与 update() 互斥。
    //
    // ⚠️ 本方法**仅供测试接缝使用**，其可见性由编译期宏 RK_ENABLE_TRACK_RESET_SEAM 约束——
    //    未定义该宏的 target 里 reset() 根本不存在（调用即编译失败），故生产目标无法误用。
    //    该宏**不改变 ABI**（对比 RK_ENABLE_TEST_SEAMS：后者会让 Engine 的析构由非虚变虚、
    //    多出 vptr，属 ABI 变更）；本宏只增删一个成员函数声明，不改变对象布局。
    //    允许携带该宏的 target 白名单由 CMake 侧的接缝白名单配置期断言强制（见 cmake/core.cmake）。
#if defined(RK_ENABLE_TRACK_RESET_SEAM)
    void reset();
#endif

    // 按图库有效身份**精准撤销**（第三波审查 #1：reset() 过宽——多人场景删除/注册
    // **一人**会瞬时抹掉其他人的稳定身份、需重新累计 stableFrames）。
    // 语义：对 stableId **或候选身份（candidateId）不在** keepIds（本次图库仍有效的身份集合）
    // 中的轨迹，清掉其稳定值（清为 "Unknown"、清零 stableConfidence）与候选（candidateId/candidateHits）；
    // **保留轨迹本体、bbox 与框确认/缺失计数**，避免丢框、避免重新累计 stableFrames。
    // A2：候选身份是「正在累计、尚未确认」的另一半身份真相，必须一并按 keepIds 撤销——否则被删除
    //   者的候选会继续累计并在第 3 次「确认」出一个已不在图库的身份。keepIds 中的身份（稳定值或候选）
    //   原样保留。
    // 空 keepIds（删除末位人员 → 空图库）→ 全部清为 "Unknown"（含清空候选），与 reset() 的全清身份
    // 语义等价，但**不丢框**（reset() 会清空轨迹、需重新累计确认）。
    // 线程安全：与 reset() 一致，非线程安全；调用方（Engine）须持 tracksMu_ 与 update() 互斥。
    void resetIdentitiesExcept(const std::vector<std::string>& keepIds);

    int trackCount() const { return static_cast<int>(tracks_.size()); }

private:
    static float iou(const cv::Rect& a, const cv::Rect& b);

    // 中心距离 + 尺寸比回退匹配（仅在 IoU 阈值不足时使用，避免相邻脸误合并）。
    // 任务 C2：由匿名 namespace 的常量化自由函数改为**成员函数**，以便读取 config_ 里的阈值
    // （阈值现为可注入配置；默认值与原常量相同）。
    bool centerSizeMatched(const cv::Rect& ref, const cv::Rect& det) const;

    std::vector<TrackView> views() const;

    // 内部轨迹状态：对外 TrackView + 确认/缺失计数（任务5 确认与消失规则）
    struct InternalTrack {
        TrackView view;
        int confirmFrames = 0;  // 连续实际检测命中帧数（**框**寿命，与身份确认相互独立）
        int missedActual = 0;   // 连续实际检测缺失帧数
        // A2：身份与「框」解耦。candidateId 为当前正在累计的身份，candidateHits 为其**连续实际
        // 识别**次数；连续（固定门槛）3 次同一身份才写入 view.stableId，否则保持 "Unknown"。
        // 换人 / 未识别 / 识别失败 / 实际漏检一律 clearIdentity（清空二者）后重新累计。
        std::string candidateId;
        int candidateHits = 0;
    };

    // A2 身份确认与撤销的两个私有辅助（声明放在 InternalTrack 定义之后，因其形参依赖该嵌套类型）：
    //   clearIdentity  —— 把轨迹的稳定身份与候选一并清为「未认证 + 无候选」；
    //   observeIdentity—— 按本帧识别结果更新候选；连续 3 次同一身份才置 stableId，否则保持 Unknown。
    static void clearIdentity(InternalTrack& t);
    static void observeIdentity(InternalTrack& t, const DetectedFace& face);

    TrackConfig config_;
    std::vector<InternalTrack> tracks_;
    int nextTrackId_ = 1;
};

}  // namespace pipeline
