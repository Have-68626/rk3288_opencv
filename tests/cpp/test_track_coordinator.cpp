#include "pipeline/TrackCoordinator.h"
#include <gtest/gtest.h>

#include <vector>

namespace {

pipeline::DetectedFace face(int x, int y, int w, int h,
                            bool authenticated = false,
                            const std::string& id = "",
                            float conf = 0.85f) {
    pipeline::DetectedFace f;
    f.bbox = cv::Rect(x, y, w, h);
    f.identityId = id;
    f.confidence = conf;
    f.isAuthenticated = authenticated;
    return f;
}

}  // namespace

// 首次检测：新建未确认轨迹（taskId=1，身份透传）
TEST(TrackCoordinator, CreatesNewTrackForFirstFace) {
    pipeline::TrackCoordinator tc;
    auto tracks = tc.update({face(10, 20, 100, 150)}, 1000, true);
    ASSERT_EQ(tracks.size(), 1);
    EXPECT_EQ(tracks[0].trackId, 1);
    EXPECT_EQ(tracks[0].stableId, "Unknown");
    EXPECT_EQ(tracks[0].lastSeenMs, 1000);
}

// 任务5：未确认轨迹在下一次实际空检测即删（不再仅靠 TTL 保留空框）
TEST(TrackCoordinator, UnconfirmedTrackRemovedOnNextEmptyDetection) {
    pipeline::TrackCoordinator tc;
    auto t1 = tc.update({face(10, 20, 100, 150)}, 1000, true);
    ASSERT_EQ(t1.size(), 1);
    auto t2 = tc.update({}, 1100, true);
    EXPECT_TRUE(t2.empty());
}

// IoU 连续帧匹配同一轨迹
TEST(TrackCoordinator, IoUMatchesConsecutiveFrames) {
    pipeline::TrackCoordinator tc;
    auto tracks1 = tc.update({face(10, 20, 100, 150, true, "alice", 0.92f)}, 1000, true);
    ASSERT_EQ(tracks1.size(), 1);
    EXPECT_EQ(tracks1[0].trackId, 1);
    EXPECT_EQ(tracks1[0].stableId, "alice");

    // 微移（高 IoU）
    auto tracks2 = tc.update({face(12, 22, 100, 150, true, "alice", 0.93f)}, 1100, true);
    ASSERT_EQ(tracks2.size(), 1);
    EXPECT_EQ(tracks2[0].trackId, 1);
    EXPECT_EQ(tracks2[0].stableId, "alice");
    EXPECT_EQ(tracks2[0].lastSeenMs, 1100);
}

// 认证升级：未知 → 已认证身份
TEST(TrackCoordinator, AuthenticatedUpgradesUnknown) {
    pipeline::TrackCoordinator tc;
    auto tracks1 = tc.update({face(10, 20, 100, 150)}, 1000, true);
    ASSERT_EQ(tracks1.size(), 1);
    EXPECT_EQ(tracks1[0].stableId, "Unknown");

    auto tracks2 = tc.update({face(10, 20, 100, 150, true, "bob", 0.88f)}, 1100, true);
    ASSERT_EQ(tracks2.size(), 1);
    EXPECT_EQ(tracks2[0].stableId, "bob");
    EXPECT_GT(tracks2[0].stableConfidence, 0.87f);
}

// 任务5：新轨迹连续 3 次实际检测确认后，单次缺失仍保留
TEST(TrackCoordinator, ConfirmedAfterThreeHitsSurvivesOneMiss) {
    pipeline::TrackCoordinator tc;
    for (long long ts : {1000LL, 1100LL, 1200LL}) {
        auto t = tc.update({face(10, 20, 100, 150)}, ts, true);
        ASSERT_EQ(t.size(), 1u);
    }
    auto afterMiss = tc.update({}, 1300, true);
    ASSERT_EQ(afterMiss.size(), 1u);  // 已确认：缺失 1 次不删
}

// 任务5：已确认轨迹连续缺失 2 次删除
TEST(TrackCoordinator, ConfirmedTrackRemovedAfterTwoMisses) {
    pipeline::TrackCoordinator tc;
    for (long long ts : {1000LL, 1100LL, 1200LL}) {
        ASSERT_EQ(tc.update({face(10, 20, 100, 150)}, ts, true).size(), 1u);
    }
    ASSERT_EQ(tc.update({}, 1300, true).size(), 1u);   // miss 1：保留
    EXPECT_TRUE(tc.update({}, 1400, true).empty());     // miss 2：删除
}

// 任务5：调度跳过检测不计缺失，确认轨迹持续保留
TEST(TrackCoordinator, SkippedDetectionDoesNotCountAsMiss) {
    pipeline::TrackCoordinator tc;
    for (long long ts : {1000LL, 1100LL, 1200LL}) {
        ASSERT_EQ(tc.update({face(10, 20, 100, 150)}, ts, true).size(), 1u);
    }
    // 多次跳过检测（调度节流）：不计缺失，轨迹保持
    for (long long ts = 1300; ts <= 1800; ts += 100) {
        ASSERT_EQ(tc.update({}, ts, false).size(), 1u);
    }
    // 恢复真实检测（空）：开始累计缺失，第 2 次空删除（已确认）
    ASSERT_EQ(tc.update({}, 1900, true).size(), 1u);
    EXPECT_TRUE(tc.update({}, 2000, true).empty());
}

// 任务5：IoU 不足时用中心距离+尺寸比回退匹配（快速移动 + 尺寸变化）
TEST(TrackCoordinator, CenterDistanceFallbackMatchesMovedFace) {
    pipeline::TrackCoordinator tc;
    auto t1 = tc.update({face(0, 0, 100, 100)}, 1000, true);
    ASSERT_EQ(t1.size(), 1);
    // (0,0,100,100) → (65,0,110,105)：IoU≈0.19<0.3，中心偏移≈70≤0.8*100，尺寸比 1.1/1.05
    auto t2 = tc.update({face(65, 0, 110, 105)}, 1100, true);
    ASSERT_EQ(t2.size(), 1);
    EXPECT_EQ(t2[0].trackId, t1[0].trackId);
    EXPECT_EQ(t2[0].bbox.x, 65);
}

// 任务5：中心距离回退不应把相邻真实脸误并到同一轨迹
TEST(TrackCoordinator, CenterDistanceFallbackKeepsAdjacentFacesSeparate) {
    pipeline::TrackCoordinator tc;
    auto t1 = tc.update({face(0, 0, 100, 100)}, 1000, true);
    ASSERT_EQ(t1.size(), 1);
    // 帧 2：原脸 + 明显分离的相邻脸（中心间距 150 ≫ 0.8*100）
    auto t2 = tc.update({face(0, 0, 100, 100), face(150, 20, 100, 110)}, 1100, true);
    ASSERT_EQ(t2.size(), 2);
    EXPECT_EQ(t2[0].trackId, 1);
}

// TTL 仍作长期无帧的兜底清理
TEST(TrackCoordinator, TtlStillCullsTracksAsFallback) {
    pipeline::TrackCoordinator tc;
    for (long long ts : {1000LL, 1100LL, 1200LL}) {
        ASSERT_EQ(tc.update({face(10, 20, 100, 150)}, ts, true).size(), 1u);
    }
    // 超 TTL 后即使跳过检测也清理（TTL 兜底）
    EXPECT_TRUE(tc.update({}, 1200 + 2000, false).empty());
}