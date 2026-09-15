#include "pipeline/TrackCoordinator.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
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

// 首次检测：新建未确认轨迹（内部 trackId=1，身份透传）；未确认不对外输出
TEST(TrackCoordinator, CreatesNewTrackForFirstFace) {
    pipeline::TrackCoordinator tc;
    auto first = tc.update({face(10, 20, 100, 150)}, 1000, true);
    EXPECT_TRUE(first.empty());     // 未确认（<stableFrames）不进入对外输出
    EXPECT_EQ(tc.trackCount(), 1);  // 内部轨迹已建立

    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1100, true).empty());
    auto tracks = tc.update({face(10, 20, 100, 150)}, 1200, true);  // 第 3 次命中确认
    ASSERT_EQ(tracks.size(), 1);
    EXPECT_EQ(tracks[0].trackId, 1);
    EXPECT_EQ(tracks[0].stableId, "Unknown");
    EXPECT_EQ(tracks[0].lastSeenMs, 1200);
}

// 任务5：未确认轨迹在下一次实际空检测即删（不再仅靠 TTL 保留空框）
TEST(TrackCoordinator, UnconfirmedTrackRemovedOnNextEmptyDetection) {
    pipeline::TrackCoordinator tc;
    auto t1 = tc.update({face(10, 20, 100, 150)}, 1000, true);
    EXPECT_TRUE(t1.empty());        // 未确认不输出
    EXPECT_EQ(tc.trackCount(), 1);
    auto t2 = tc.update({}, 1100, true);
    EXPECT_TRUE(t2.empty());
    EXPECT_EQ(tc.trackCount(), 0);  // 未确认：下一次实际检测缺失即删
}

// IoU 连续帧匹配同一轨迹（3 帧确认后才对外输出）
TEST(TrackCoordinator, IoUMatchesConsecutiveFrames) {
    pipeline::TrackCoordinator tc;
    auto tracks1 = tc.update({face(10, 20, 100, 150, true, "alice", 0.92f)}, 1000, true);
    EXPECT_TRUE(tracks1.empty());
    EXPECT_EQ(tc.trackCount(), 1);

    // 微移（高 IoU）：仍是同一轨迹，未新建
    auto tracks2 = tc.update({face(12, 22, 100, 150, true, "alice", 0.93f)}, 1100, true);
    EXPECT_TRUE(tracks2.empty());
    EXPECT_EQ(tc.trackCount(), 1);

    auto tracks3 = tc.update({face(12, 22, 100, 150, true, "alice", 0.93f)}, 1200, true);
    ASSERT_EQ(tracks3.size(), 1);
    EXPECT_EQ(tc.trackCount(), 1);
    EXPECT_EQ(tracks3[0].trackId, 1);
    EXPECT_EQ(tracks3[0].stableId, "alice");
    EXPECT_EQ(tracks3[0].lastSeenMs, 1200);
}

// 认证升级：未知 → 已认证身份（第 3 帧认证并确认后输出）
TEST(TrackCoordinator, AuthenticatedUpgradesUnknown) {
    pipeline::TrackCoordinator tc;
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1100, true).empty());

    auto tracks3 = tc.update({face(10, 20, 100, 150, true, "bob", 0.88f)}, 1200, true);
    ASSERT_EQ(tracks3.size(), 1);
    EXPECT_EQ(tracks3[0].stableId, "bob");
    EXPECT_GT(tracks3[0].stableConfidence, 0.87f);
}

// 任务5：新轨迹连续 3 次实际检测确认后，单次缺失仍保留
TEST(TrackCoordinator, ConfirmedAfterThreeHitsSurvivesOneMiss) {
    pipeline::TrackCoordinator tc;
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1100, true).empty());
    ASSERT_EQ(tc.update({face(10, 20, 100, 150)}, 1200, true).size(), 1u);
    auto afterMiss = tc.update({}, 1300, true);
    ASSERT_EQ(afterMiss.size(), 1u);  // 已确认：缺失 1 次不删
}

// 任务5：已确认轨迹连续缺失 2 次删除
TEST(TrackCoordinator, ConfirmedTrackRemovedAfterTwoMisses) {
    pipeline::TrackCoordinator tc;
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1100, true).empty());
    ASSERT_EQ(tc.update({face(10, 20, 100, 150)}, 1200, true).size(), 1u);
    ASSERT_EQ(tc.update({}, 1300, true).size(), 1u);   // miss 1：保留
    EXPECT_TRUE(tc.update({}, 1400, true).empty());     // miss 2：删除
}

// 任务5：调度跳过检测不计缺失，确认轨迹持续保留
TEST(TrackCoordinator, SkippedDetectionDoesNotCountAsMiss) {
    pipeline::TrackCoordinator tc;
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1100, true).empty());
    ASSERT_EQ(tc.update({face(10, 20, 100, 150)}, 1200, true).size(), 1u);
    // 多次跳过检测（调度节流）：不计缺失，轨迹保持
    for (long long ts = 1300; ts <= 1800; ts += 100) {
        ASSERT_EQ(tc.update({}, ts, false).size(), 1u);
    }
    // 恢复真实检测（空）：开始累计缺失，第 2 次空删除（已确认）
    ASSERT_EQ(tc.update({}, 1900, true).size(), 1u);
    EXPECT_TRUE(tc.update({}, 2000, true).empty());
}

// 任务5：IoU 不足时用中心距离+尺寸比回退匹配（快速移动/尺寸变化致 IoU 下降）
TEST(TrackCoordinator, CenterDistanceFallbackMatchesMovedFace) {
    pipeline::TrackCoordinator tc;
    // 连续 3 帧稳定建立已确认轨迹 (0,0,100,100)
    EXPECT_TRUE(tc.update({face(0, 0, 100, 100)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(0, 0, 100, 100)}, 1100, true).empty());
    ASSERT_EQ(tc.update({face(0, 0, 100, 100)}, 1200, true).size(), 1u);
    // (0,0,100,100) → (65,0,110,105)：大位移使 IoU≈0.19<0.3，
    // 中心偏移≈70.2 ≤ 0.8*100=80，尺寸比 1.1/1.05 ∈ [0.5,2.0] → 回退匹配同一轨迹
    auto t2 = tc.update({face(65, 0, 110, 105)}, 1300, true);
    ASSERT_EQ(t2.size(), 1);
    EXPECT_EQ(t2[0].trackId, 1);
    EXPECT_EQ(t2[0].bbox.x, 65);
}

// 任务5：中心距离回退不应把相邻真实脸误并到同一轨迹
TEST(TrackCoordinator, CenterDistanceFallbackKeepsAdjacentFacesSeparate) {
    pipeline::TrackCoordinator tc;
    // 帧 1：仅 A 建轨（未确认）
    ASSERT_TRUE(tc.update({face(0, 0, 100, 100)}, 1000, true).empty());
    // 帧 2：A + 明显分离的相邻脸 B（中心间距 150 ≫ 0.25*100）→ B 另建轨迹，不并入 A
    tc.update({face(0, 0, 100, 100), face(150, 20, 100, 110)}, 1100, true);
    EXPECT_EQ(tc.trackCount(), 2);
    // 再累积两帧使两者确认，输出 2 条独立轨迹，A 保持 trackId=1
    tc.update({face(0, 0, 100, 100), face(150, 20, 100, 110)}, 1200, true);
    auto t4 = tc.update({face(0, 0, 100, 100), face(150, 20, 100, 110)}, 1300, true);
    ASSERT_EQ(t4.size(), 2u);
    EXPECT_EQ(t4[0].trackId, 1);
    EXPECT_NE(t4[1].trackId, 1);
}

// TTL 仍作长期无帧的兜底清理
TEST(TrackCoordinator, TtlStillCullsTracksAsFallback) {
    pipeline::TrackCoordinator tc;
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1100, true).empty());
    ASSERT_EQ(tc.update({face(10, 20, 100, 150)}, 1200, true).size(), 1u);
    // 超 TTL 后即使跳过检测也清理（TTL 兜底）
    EXPECT_TRUE(tc.update({}, 1200 + 2000, false).empty());
}

// 第三波审查 #2：reset() 清空全部内部轨迹（撤销稳定身份），但保留 nextTrackId_ 不回退，
// 使新轨迹不复用已发布过的 trackId（避免同 ID 先后指向不同目标）。
TEST(TrackCoordinator, ResetClearsTracksButKeepsIdCounter) {
    pipeline::TrackCoordinator tc;
    // 建立并确认一条已认证轨迹（trackId 从 1 起）
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150, true, "alice", 0.90f)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150, true, "alice", 0.90f)}, 1100, true).empty());
    auto confirmed = tc.update({face(10, 20, 100, 150, true, "alice", 0.90f)}, 1200, true);
    ASSERT_EQ(confirmed.size(), 1u);
    ASSERT_EQ(confirmed[0].trackId, 1);
    ASSERT_EQ(confirmed[0].stableId, "alice");
    EXPECT_EQ(tc.trackCount(), 1);

    // reset 后内部轨迹清空，对外不再输出任何轨迹（旧身份 alice 作废）
    tc.reset();
    EXPECT_EQ(tc.trackCount(), 0);
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150, true, "alice", 0.90f)}, 1300, true).empty());
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150, true, "alice", 0.90f)}, 1400, true).empty());

    // reset 后再次确认的新轨迹使用全新 trackId（2，而非复用 1），且身份须重新确认后才输出
    auto rebuilt = tc.update({face(10, 20, 100, 150, true, "alice", 0.90f)}, 1500, true);
    ASSERT_EQ(rebuilt.size(), 1u);
    EXPECT_EQ(rebuilt[0].trackId, 2);
    EXPECT_EQ(rebuilt[0].stableId, "alice");
}

// 第三波审查 #1：resetIdentitiesExcept 精准撤销——仅清「已不在图库」的身份，
// 保留其他轨迹身份与轨迹本体（bbox），不丢框、不重新累计 stableFrames。
TEST(TrackCoordinator, ResetIdentitiesExceptKeepsOtherIdentitiesAndBoxes) {
    pipeline::TrackCoordinator tc;
    // 3 帧确认两条已认证轨迹：alice（左）/ bob（右）
    for (int i = 0; i < 3; ++i) {
        tc.update({face(0, 0, 100, 150, true, "alice", 0.90f),
                   face(200, 0, 100, 150, true, "bob", 0.91f)},
                  1000 + i * 100, true);
    }
    ASSERT_EQ(tc.trackCount(), 2);

    // 模拟「仅删除 alice」：keepIds={bob}
    tc.resetIdentitiesExcept({"bob"});

    // 空检测（已确认轨迹单次缺失仍保留）→ 观测内部身份
    const auto views = tc.update({}, 1400, true);
    EXPECT_EQ(tc.trackCount(), 2);  // 轨迹本体保留，不丢框
    ASSERT_EQ(views.size(), 2u);
    std::vector<std::string> ids;
    for (const auto& v : views) {
        EXPECT_GT(v.bbox.area(), 0);  // bbox 保留
        ids.push_back(v.stableId);
    }
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], "Unknown");  // alice 身份被撤销
    EXPECT_EQ(ids[1], "bob");      // bob 身份原样保留
}

// 第三波审查 #1：空 keepIds（删除末位人员 → 空图库）→ 全部清为 "Unknown"，
// 与 reset() 的全清语义等价，但**不丢框**（轨迹本体保留；reset() 则会清空轨迹）。
TEST(TrackCoordinator, ResetIdentitiesExceptEmptyClearsAllButKeepsBoxes) {
    pipeline::TrackCoordinator tc;
    for (int i = 0; i < 3; ++i) {
        tc.update({face(0, 0, 100, 150, true, "alice", 0.90f),
                   face(200, 0, 100, 150, true, "bob", 0.91f)},
                  1000 + i * 100, true);
    }
    ASSERT_EQ(tc.trackCount(), 2);

    tc.resetIdentitiesExcept({});  // 空图库

    const auto views = tc.update({}, 1400, true);
    EXPECT_EQ(tc.trackCount(), 2);  // 不丢框（对比：reset() 后 trackCount 归零）
    ASSERT_EQ(views.size(), 2u);
    for (const auto& v : views) EXPECT_EQ(v.stableId, "Unknown");
}
