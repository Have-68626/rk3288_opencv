#include "pipeline/TrackCoordinator.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
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

TEST(TrackCoordinator, LowercaseUnknownNeverConfirmsAndRevokesAnExistingIdentity) {
    pipeline::TrackConfig cfg;
    cfg.stableFrames = 1;
    pipeline::TrackCoordinator coordinator(cfg);
    std::vector<pipeline::TrackView> views;
    for (int i = 0; i < 3; ++i) {
        views = coordinator.update({face(10, 10, 80, 80, true, "unknown")}, 1000 + i * 100, true);
    }
    ASSERT_EQ(views.size(), 1u);
    EXPECT_EQ(views[0].stableId, "Unknown");
    for (int i = 0; i < 3; ++i) {
        views = coordinator.update({face(10, 10, 80, 80, true, "registered")}, 1400 + i * 100, true);
    }
    ASSERT_EQ(views.size(), 1u);
    ASSERT_EQ(views[0].stableId, "registered");
    views = coordinator.update({face(10, 10, 80, 80, true, "unknown")}, 1800, true);
    ASSERT_EQ(views.size(), 1u);
    EXPECT_EQ(views[0].stableId, "Unknown");
    EXPECT_FLOAT_EQ(views[0].stableConfidence, 0.0f);
}

TEST(TrackCoordinator, GalleryRevocationPreservesKeptPendingCandidateInReadOnlyViews) {
    pipeline::TrackConfig cfg;
    cfg.stableFrames = 1;
    pipeline::TrackCoordinator coordinator(cfg);
    const auto initial = coordinator.update({
        face(0, 0, 40, 40, true, "candidate_a"),
        face(120, 0, 40, 40, true, "candidate_b")}, 1000, true);
    ASSERT_EQ(initial.size(), 2u);
    ASSERT_TRUE(initial[0].identityPending);
    ASSERT_TRUE(initial[1].identityPending);
    coordinator.resetIdentitiesExcept({"candidate_b"});
    const auto snapshot = coordinator.views();
    ASSERT_EQ(snapshot.size(), 2u);
    EXPECT_FALSE(snapshot[0].identityPending);
    EXPECT_TRUE(snapshot[1].identityPending);
    const auto skipped = coordinator.update({}, 1100, false);
    ASSERT_EQ(skipped.size(), snapshot.size());
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        EXPECT_EQ(snapshot[i].trackId, skipped[i].trackId);
        EXPECT_EQ(snapshot[i].bbox.x, skipped[i].bbox.x);
        EXPECT_EQ(snapshot[i].stableId, skipped[i].stableId);
        EXPECT_EQ(snapshot[i].identityPending, skipped[i].identityPending);
    }
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

// A2 新产品契约：认证升级——**框**在第 3 次命中即确认输出，但身份不再是「首帧认证即写入」，
// 而是「同一身份连续 3 次实际识别」才 VERIFIED。故单帧认证不升级；须连续 3 帧同一身份。
// 判别点：删掉 observeIdentity 的 3 次门槛（改回「stableId=="Unknown" 且 isAuthenticated 即写入」）
//        ⇒ 第 3 帧的 Unknown 断言翻红。
TEST(TrackCoordinator, AuthenticatedUpgradesUnknown) {
    pipeline::TrackCoordinator tc;
    // 两帧匿名：框未确认、无身份候选
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1100, true).empty());

    // 第 3 帧认证 bob：框确认并输出，但身份尚在累计（1/3）→ Unknown
    auto u1 = tc.update({face(10, 20, 100, 150, true, "bob", 0.88f)}, 1200, true);
    ASSERT_EQ(u1.size(), 1u);
    EXPECT_EQ(u1[0].stableId, "Unknown") << "单帧认证不得立即升级（A2：需连续 3 次实际识别）";

    // 第 4 帧继续 bob：仍 Unknown（2/3）
    auto u2 = tc.update({face(10, 20, 100, 150, true, "bob", 0.88f)}, 1300, true);
    ASSERT_EQ(u2.size(), 1u);
    EXPECT_EQ(u2[0].stableId, "Unknown");

    // 第 5 帧：连续第 3 次同一身份 → 升级为 bob
    auto u3 = tc.update({face(10, 20, 100, 150, true, "bob", 0.89f)}, 1400, true);
    ASSERT_EQ(u3.size(), 1u);
    EXPECT_EQ(u3[0].stableId, "bob");
    EXPECT_GT(u3[0].stableConfidence, 0.87f);
}

// ── A2：同身份连续 3 次实际识别才 VERIFIED；换人/未识别/识别失败/实际漏检立即撤销 ──────

// 同位置换人：旧身份**立即撤销**（不再黏住），新人满 3 次识别后接管同一轨迹。
// 判别点：删掉 observeIdentity 里「candidateId != identityId → clearIdentity」的撤销分支
//        ⇒ 换人首帧仍报 alice ⇒ 本用例 b1 断言翻红。
TEST(TrackCoordinator, SamePositionSwitchRevokesIdentityImmediately) {
    pipeline::TrackCoordinator tc;
    tc.update({face(10, 20, 100, 150, true, "test_person_a")}, 1000, true);
    tc.update({face(10, 20, 100, 150, true, "test_person_a")}, 1100, true);
    auto a = tc.update(
        {face(10, 20, 100, 150, true, "test_person_a")}, 1200, true);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].stableId, "test_person_a");
    const int id = a[0].trackId;
    auto b1 = tc.update(
        {face(10, 20, 100, 150, true, "test_person_b")}, 1300, true);
    ASSERT_EQ(b1.size(), 1u);
    EXPECT_EQ(b1[0].trackId, id);
    EXPECT_EQ(b1[0].stableId, "Unknown");
    auto b2 = tc.update(
        {face(10, 20, 100, 150, true, "test_person_b")}, 1400, true);
    ASSERT_EQ(b2.size(), 1u);
    EXPECT_EQ(b2[0].stableId, "Unknown");
    auto b3 = tc.update(
        {face(10, 20, 100, 150, true, "test_person_b")}, 1500, true);
    ASSERT_EQ(b3.size(), 1u);
    EXPECT_EQ(b3[0].trackId, id);
    EXPECT_EQ(b3[0].stableId, "test_person_b");
}

// 识别失败后重新确认：已确认身份因一次「未识别」立即撤销，之后须再连续 3 次实际识别才恢复。
TEST(TrackCoordinator, SameIdentityFailureRevokesThenReconfirmsAfterThreeHits) {
    pipeline::TrackCoordinator tc;
    tc.update({face(10, 20, 100, 150, true, "alice")}, 1000, true);
    tc.update({face(10, 20, 100, 150, true, "alice")}, 1100, true);
    auto a = tc.update({face(10, 20, 100, 150, true, "alice")}, 1200, true);
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(a[0].stableId, "alice");

    // 一次识别失败（同位置检测未认证）→ 立即撤销
    auto f = tc.update({face(10, 20, 100, 150)}, 1300, true);
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(f[0].stableId, "Unknown") << "识别失败须立即撤销旧身份";

    // 再 A/A/A：第三次重新确认
    ASSERT_EQ(tc.update({face(10, 20, 100, 150, true, "alice")}, 1400, true)[0].stableId, "Unknown");
    ASSERT_EQ(tc.update({face(10, 20, 100, 150, true, "alice")}, 1500, true)[0].stableId, "Unknown");
    auto r3 = tc.update({face(10, 20, 100, 150, true, "alice")}, 1600, true);
    ASSERT_EQ(r3.size(), 1u);
    EXPECT_EQ(r3[0].stableId, "alice") << "失败后须重新连续 3 次才再次确认";
}

// 一次实际漏检：框按旧寿命保留（已确认缺 1 次不删），但身份立即撤销。
TEST(TrackCoordinator, IdentityRevokedOnSingleActualMissWhileBoxSurvives) {
    pipeline::TrackCoordinator tc;
    tc.update({face(10, 20, 100, 150, true, "alice")}, 1000, true);
    tc.update({face(10, 20, 100, 150, true, "alice")}, 1100, true);
    auto a = tc.update({face(10, 20, 100, 150, true, "alice")}, 1200, true);
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(a[0].stableId, "alice");

    auto miss = tc.update({}, 1300, true);
    ASSERT_EQ(miss.size(), 1u) << "已确认框缺 1 次仍续航输出";
    EXPECT_EQ(miss[0].stableId, "Unknown") << "实际漏检须立即撤销身份";
    EXPECT_GT(miss[0].bbox.area(), 0) << "框本体保留";
}

// 两次识别之间跳过检测：跳过不累计身份次数，第三次**实际**识别才确认。
TEST(TrackCoordinator, SkippedDetectionDoesNotAccumulateIdentityHits) {
    pipeline::TrackCoordinator tc;
    // 匿名 1 帧（框 confirm=1）+ 认证 1 帧（confirm=2，候选 alice=1）
    tc.update({face(10, 20, 100, 150)}, 1000, true);
    tc.update({face(10, 20, 100, 150, true, "alice")}, 1100, true);
    // 跳过检测（detectionRan=false）：不计缺失、也不计入身份识别次数
    tc.update({}, 1150, false);
    // 第 3 个真实命中帧：框确认输出，但身份仅累计到 2 次 → Unknown
    auto v2 = tc.update({face(10, 20, 100, 150, true, "alice")}, 1200, true);
    ASSERT_EQ(v2.size(), 1u);
    EXPECT_EQ(v2[0].stableId, "Unknown") << "跳过检测不得计入身份识别次数";
    // 再来一次**实际**识别：满 3 次 → alice
    auto v3 = tc.update({face(10, 20, 100, 150, true, "alice")}, 1300, true);
    ASSERT_EQ(v3.size(), 1u);
    EXPECT_EQ(v3[0].stableId, "alice");
}

// 存在性审查缺口：直接断言「已 VERIFIED 的身份在 detectionRan=false（跳过检测）帧不撤销」。
// 既有两例都不是直接判据：SkippedDetectionDoesNotAccumulateIdentityHits 在身份**尚未 VERIFIED**
// 时跳过；TtlExpiresConfirmedIdentityDuringSkippedDetection 走的是 TTL 兜底清理。
// 判别点：把「跳过检测」分支误改成撤销身份（例如 `if (!detectionRan)` 内先 clearIdentity 再 return）
//        ⇒ 跳过帧读到 Unknown ⇒ 本用例红；还原 ⇒ 跳过帧仍读 alice ⇒ 绿。
TEST(TrackCoordinator, VerifiedIdentitySurvivesSkippedDetectionFrame) {
    pipeline::TrackCoordinator tc;
    // 连续 3 次**实际**识别 → 身份 VERIFIED 为 alice
    tc.update({face(10, 20, 100, 150, true, "alice")}, 1000, true);
    tc.update({face(10, 20, 100, 150, true, "alice")}, 1100, true);
    auto verified = tc.update({face(10, 20, 100, 150, true, "alice")}, 1200, true);
    ASSERT_EQ(verified.size(), 1u);
    ASSERT_EQ(verified[0].stableId, "alice");

    // 跳过检测帧（detectionRan=false，且在 TTL 内）：既不计缺失、也**不得**撤销已 VERIFIED 身份
    auto skipped = tc.update({}, 1300, false);
    ASSERT_EQ(skipped.size(), 1u) << "跳过检测帧仍应输出已确认轨迹";
    EXPECT_EQ(skipped[0].stableId, "alice")
            << "detectionRan=false 不得撤销已 VERIFIED 的身份（候选/命中计数亦不得被清）";

    // 恢复实际识别：因跳过帧未清身份与候选，本帧应即时仍为 alice（无需重新累计 3 次）
    auto resumed = tc.update({face(10, 20, 100, 150, true, "alice")}, 1400, true);
    ASSERT_EQ(resumed.size(), 1u);
    EXPECT_EQ(resumed[0].stableId, "alice") << "跳过帧不得清空候选，恢复帧应即时仍为 alice";
}

// 跳过期间 TTL 已到：轨迹仍按 TTL 兜底清理（顺序不变量：TTL 在跳过早退之前）。
TEST(TrackCoordinator, TtlExpiresConfirmedIdentityDuringSkippedDetection) {
    pipeline::TrackConfig cfg{};
    cfg.ttlMs = 200;
    pipeline::TrackCoordinator tc(cfg);
    tc.update({face(10, 20, 100, 150, true, "alice")}, 1000, true);
    tc.update({face(10, 20, 100, 150, true, "alice")}, 1100, true);
    ASSERT_EQ(tc.update({face(10, 20, 100, 150, true, "alice")}, 1200, true).size(), 1u);
    EXPECT_TRUE(tc.update({}, 1200 + 300, false).empty())
            << "跳过期间超过 TTL 仍须兜底清理";
}

// A/B 两轨，只撤销 A：B 保留，A 的稳定身份**与候选**一并清（否则单帧命中会以存量 3 次复活）。
TEST(TrackCoordinator, ResetIdentitiesExceptClearsRevokedCandidateButKeepsOther) {
    pipeline::TrackCoordinator tc;
    for (int i = 0; i < 3; ++i) {
        tc.update({face(0, 0, 100, 150, true, "alice", 0.90f),
                   face(200, 0, 100, 150, true, "bob", 0.90f)},
                  1000 + i * 100, true);
    }
    ASSERT_EQ(tc.trackCount(), 2);
    tc.resetIdentitiesExcept({"bob"});
    // 观测：喂一帧两框认证。若 alice 候选未被清（存量 3 次），本帧会立刻重新确认 alice。
    const auto v = tc.update({face(0, 0, 100, 150, true, "alice", 0.90f),
                              face(200, 0, 100, 150, true, "bob", 0.90f)},
                             1300, true);
    ASSERT_EQ(v.size(), 2u);
    std::vector<std::string> ids;
    for (const auto& t : v) ids.push_back(t.stableId);
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids[0], "Unknown") << "被撤销者的候选也须清零，不得凭存量重新确认";
    EXPECT_EQ(ids[1], "bob") << "未撤销者身份保留";
}

// 两次候选 A 后图库删除 A：该帧检测被代次作废降级为未认证，不得据旧候选完成确认。
TEST(TrackCoordinator, StaleGenerationDowngradeCannotReconfirmIdentity) {
    pipeline::TrackCoordinator tc;
    tc.update({face(10, 20, 100, 150, true, "alice")}, 1000, true);  // 候选 1
    tc.update({face(10, 20, 100, 150, true, "alice")}, 1100, true);  // 候选 2
    // 第 3 帧本应确认，但图库代次已变 → Engine 已把检测降级为「未认证 + Unknown」（此处建模为未认证）
    auto stale = tc.update({face(10, 20, 100, 150)}, 1200, true);
    ASSERT_EQ(stale.size(), 1u);  // 框第 3 次命中被确认输出
    EXPECT_EQ(stale[0].stableId, "Unknown")
            << "代次作废帧不得据旧候选完成身份确认";
    // 之后重新认证 A：候选已清零，须重新累计（本帧仍 Unknown）
    auto r = tc.update({face(10, 20, 100, 150, true, "alice")}, 1300, true);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].stableId, "Unknown") << "作废后候选已清零，须重新累计满 3 次";
}

// 框连续命中、身份 A/B/A 来回切换：框可见，但身份始终保持 Unknown（每次都从头累计）。
TEST(TrackCoordinator, AlternatingIdentityKeepsBoxVisibleButIdentityUnknown) {
    pipeline::TrackCoordinator tc;
    tc.update({face(10, 20, 100, 150, true, "A")}, 1000, true);
    tc.update({face(10, 20, 100, 150, true, "A")}, 1100, true);
    auto a = tc.update({face(10, 20, 100, 150, true, "A")}, 1200, true);
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(a[0].stableId, "A");

    auto b = tc.update({face(10, 20, 100, 150, true, "B")}, 1300, true);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].stableId, "Unknown") << "换到 B 首帧应撤销 A";
    auto a2 = tc.update({face(10, 20, 100, 150, true, "A")}, 1400, true);
    ASSERT_EQ(a2.size(), 1u);
    EXPECT_EQ(a2[0].stableId, "Unknown") << "身份来回切换时框可见但身份保持 Unknown";
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

// 任务5：missedActual 命中即清零——miss 1 → hit（清零）→ miss 1 ⇒ 仍在。
// 既有三条「缺失」用例**都**是「先连续 miss 或先跳跃」，没有一条走「miss → hit → miss」，
// 故 TrackCoordinator::update 命中分支里的 `t.missedActual = 0;` 此前无直测。
// 判别点：删掉该行（命中不清零）⇒ 末帧的缺失计数累计为 2，已确认轨迹被删，末条断言（仍应 1 条）翻红。
TEST(TrackCoordinator, HitResetsMissedActualCounter) {
    pipeline::TrackCoordinator tc;
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(10, 20, 100, 150)}, 1100, true).empty());
    ASSERT_EQ(tc.update({face(10, 20, 100, 150)}, 1200, true).size(), 1u);  // 第 3 次命中：已确认
    ASSERT_EQ(tc.update({}, 1300, true).size(), 1u);                        // miss 1：missedActual=1
    ASSERT_EQ(tc.update({face(10, 20, 100, 150)}, 1400, true).size(), 1u);  // hit：必须清零
    // 时间戳仍须落在 ttlMs=1200 内（1500-1400=100），否则 TTL 兜底会把轨迹清掉、掩盖真实原因
    EXPECT_EQ(tc.update({}, 1500, true).size(), 1u)
            << "命中未清零 missedActual：本帧被当成「连续缺失 2 次」而删除了已确认轨迹";
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
    // 帧 2：A + 明显分离的相邻脸 B（中心距 ≈152.1 > 0.8*100=80，即超过
    // centerMaxRatio × max(refW,refH)）→ B 另建轨迹，不并入 A
    //（判据用的是**欧氏**中心距：A 中心 (50,50)、B 中心 (200,75)，dx=150、dy=25）
    tc.update({face(0, 0, 100, 100), face(150, 20, 100, 110)}, 1100, true);
    EXPECT_EQ(tc.trackCount(), 2);
    // 再累积两帧使两者确认，输出 2 条独立轨迹，A 保持 trackId=1
    tc.update({face(0, 0, 100, 100), face(150, 20, 100, 110)}, 1200, true);
    auto t4 = tc.update({face(0, 0, 100, 100), face(150, 20, 100, 110)}, 1300, true);
    ASSERT_EQ(t4.size(), 2u);
    EXPECT_EQ(t4[0].trackId, 1);
    EXPECT_NE(t4[1].trackId, 1);
}

// 任务5（补缺）：同一帧内**两个 det 争抢同一条已确认 track 的回退匹配**——中心距离回退分支里的
// 「已占位即跳过」(TrackCoordinator.cpp:97 `if (trackUsed[ti]) continue;`) 与「占位登记」
// (:99 `trackUsed[ti] = true;`) 这一对**此前从未被「≥2 个 det 同时落在同一 track 的回退半径内」构造过**：
// 既有的 CenterDistanceFallbackKeepsAdjacentFacesSeparate（:153-168）把第二张脸**刻意放到回退半径之外**
// （中心距 ≈152.1 > 0.8×100=80），走的是「不匹配 → 新建 track」的另一条路，从不进入同一 track 的回退循环。
// 本用例把**两张脸都放进回退半径内**、又都让 IoU 不足以走 IoU 路径，从而首次逼出这一对语句。
//
// 判别负载在 :97 与 :99 的**配对**，不在 :101 的 `break` 本身：
//   · 删掉 :97 的 `if (trackUsed[ti]) continue;`（或删掉 :99 的 `trackUsed[ti] = true;` 登记）
//     ⇒ 两个 det 都会映射到同一条 track T ⇒ 步 3 不再新建第二条 track ⇒ trackCount()==1 ⇒ 本用例 RED。
//   · **只删 :101 的 `break` 不会变红**：登记（:99）仍在，该 det 本轮已 `detToTrack[idx]=ti`；
//     即便继续循环，后续 ti 也会被 :97 的 trackUsed 挡掉，且此刻 tracks_ 只有 1 条，多绕一圈无副作用。
//     故「删 break 该红」是**错误预期**，勿据此误判本用例的判别力。
TEST(TrackCoordinator, TwoDetectionsCompeteForOneTrackInFallback) {
    pipeline::TrackCoordinator tc;
    // 帧 1000/1100/1200：单张 (0,0,100,100) 建立并确认一条 track（trackId=1，稳定确认 3 帧）。
    EXPECT_TRUE(tc.update({face(0, 0, 100, 100)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(0, 0, 100, 100)}, 1100, true).empty());
    ASSERT_EQ(tc.update({face(0, 0, 100, 100)}, 1200, true).size(), 1u);

    // 帧 1300：两个 det 对 T 的 IoU 都 < 0.3（走不进 IoU 路径），但中心距都 ≤ 80（进得了回退）：
    //   A = (60,0,100,100)：中心 (110,50)，dx=60、dy=0 → 中心距 60；与 T 交 40×100=4000，
    //        并集 10000+10000-4000=16000 → IoU = 4000/16000 = 0.25 < 0.3；尺寸比 100/100 = 1.0 ∈ [0.5,2.0]。
    //   B = (0,60,100,100)：中心 (50,110)，dx=0、dy=60 → 中心距 60；与 T 交 100×40=4000，
    //        并集同为 16000 → IoU = 0.25 < 0.3；尺寸比同样 1.0 ∈ [0.5,2.0]。
    // 两者面积都是 10000 ⇒ sort(order)（:78，非稳定）无论谁先谁后结论一致，断言不依赖排序结果。
    tc.update({face(60, 0, 100, 100), face(0, 60, 100, 100)}, 1300, true);
    // 第一个 det 占用 T（:99 登记），第二个 det 再进回退循环时被 :97 挡掉 → 不匹配 → 步 3 新建第二条 track。
    EXPECT_EQ(tc.trackCount(), 2)
            << "两个 det 争抢同一条 track 时，第二个 det 应另建轨迹而非被吞（:97/:99 的配对失效）";

    // 再累积两帧同形（新 track 需再命中 2 次才达 stableFrames=3）。
    tc.update({face(60, 0, 100, 100), face(0, 60, 100, 100)}, 1400, true);
    auto t5 = tc.update({face(60, 0, 100, 100), face(0, 60, 100, 100)}, 1500, true);
    ASSERT_EQ(t5.size(), 2u) << "两张脸都应被确认输出（第二张脸未被吞掉）";
    EXPECT_NE(t5[0].trackId, t5[1].trackId) << "两条 track 必须是不同 trackId";
    // 两条 bbox 须互不相同：一条落在 +x（60,0），一条落在 +y（0,60）。用逐字段计数而非 Rect 比较——
    // mock OpenCV（RK_SKIP_OPENCV）的 cv::Rect 未定义 operator==/!=（见 deps/opencv/.../core/types.hpp）。
    int xMoved = 0, yMoved = 0;
    for (const auto& v : t5) {
        if (v.bbox.x == 60 && v.bbox.y == 0) ++xMoved;
        if (v.bbox.x == 0 && v.bbox.y == 60) ++yMoved;
    }
    EXPECT_EQ(xMoved, 1) << "应恰有一条轨迹落在 +x 位移的 (60,0)";
    EXPECT_EQ(yMoved, 1) << "应恰有一条轨迹落在 +y 位移的 (0,60)";
}

// 任务 A（FACE-TRACK-* 补缺）：`TrackCoordinator` 回退匹配循环里那个 `break` 的**判别负载**。
//
// 既有 TwoDetectionsCompeteForOneTrackInFallback 只构造了「两个 det 争抢同一条 track」，
// 并自陈「删掉 break 不红」——此刻 tracks_ 仅 1 条，(删 break 后)多绕的一圈被回退循环开头的
// `trackUsed` 检查挡掉、又没有第二条可命中，结果不变。本用例**反过来**：让**一条 det 同时落在两条 track 的
// 回退半径内**（tracks_ ≥ 2），使 `break` 成为「取**第一个**命中的 track」与「取**最后一个**
// 命中的 track」之别：
//   · 保留 break：该 det 挂到第一个未占用的命中 track（ti 最小者，此处 T1 / trackId=1）；
//   · 删掉 break：登记 `trackUsed` 后循环继续，后续**另一条**同样命中并覆写 `detToTrack[idx]`
//     ⇒ 该 det 改挂到**最后**命中的 track（此处 T2 / trackId=2）。
// ⇒ 可观测差异：该 det 的 bbox 落到不同 trackId 上（一条被更新、另一条记一次缺失）。
//   断言「trackId==1 的轨迹拿到该 det 的 bbox」⇒ 删 break 后 trackId 1 保持旧 bbox ⇒ 红。
TEST(TrackCoordinator, OneDetectionMatchingTwoTracksFallbackBreakIsLoadBearing) {
    pipeline::TrackCoordinator tc;
    // 帧 1000/1100/1200：两条互不相交（仅共享边界 x=100、IoU=0）的已确认轨迹
    // T1(0,0,100,100)（中心 50,50）、T2(100,0,100,100)（中心 150,50），两中心相距 100。
    // 每帧两脸各自 IoU 恰好命中本人的 track（不经回退循环），3 帧后均达 stableFrames 而确认。
    for (int i = 0; i < 3; ++i) {
        tc.update({face(0, 0, 100, 100), face(100, 0, 100, 100)}, 1000 + i * 100, true);
    }
    ASSERT_EQ(tc.trackCount(), 2) << "应建立两条独立轨迹";
    // 第 4 帧仍喂两脸，确认对外输出两条（trackId 1/2 各一）。
    auto built = tc.update({face(0, 0, 100, 100), face(100, 0, 100, 100)}, 1300, true);
    ASSERT_EQ(built.size(), 2u);

    // 帧 1400：单个 det D(70,20,60,60)，中心 (100,50)——恰在 T1、T2 中心连线的中点上。
    //   · 与 T1(中心 50,50)：中心距 50 ≤ 0.8×100=80；交 30×60=1800，并 10000+3600-1800=11800
    //     ⇒ IoU≈0.153 < 0.3（走不进 IoU 路径）；尺寸比 60/100=0.6 ∈ [0.5,2.0] ⇒ **回退命中**。
    //   · 与 T2(中心 150,50)：中心距 50 ≤ 80；IoU 同为 ≈0.153 < 0.3；尺寸比 0.6 ∈ 界内 ⇒ **亦命中**。
    // 故 D 同时落在两条 track 的回退半径内，唯一变量就是 break 取哪一个。
    auto after = tc.update({face(70, 20, 60, 60)}, 1400, true);
    ASSERT_EQ(after.size(), 2u) << "两条已确认轨迹单次缺失 1 次仍应各自输出";

    int t1x = -1, t2x = -1;
    for (const auto& v : after) {
        if (v.trackId == 1) t1x = v.bbox.x;
        if (v.trackId == 2) t2x = v.bbox.x;
    }
    ASSERT_NE(t1x, -1) << "应输出 trackId=1";
    ASSERT_NE(t2x, -1) << "应输出 trackId=2";
    // 保留 break ⇒ D 挂到第一个命中的 T1（T1.bbox.x 变为 70、T2 保持 100）；
    // 删 break ⇒ 覆写到 T2（T1 保持 x=0、T2.x 变为 70）。
    EXPECT_EQ(t1x, 70)
            << "回退循环未取第一个命中的 track：break 早退失效（det 被挂到了最后一条命中轨迹）";
    EXPECT_EQ(t2x, 100) << "trackId=2 应保持原 bbox.x=100（未被 det 覆写）";
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
// A2 改口径：**不能用空检测观测内部身份**——A2 下实际漏检会立即撤销**全部**身份，
//   从而无法区分「resetIdentitiesExcept 撤销了 alice」与「漏检撤销了 bob」。故改用一帧
//   仍命中两框的实际检测来观测：alice 身份已被精准撤销（重新从头累计候选→Unknown），
//   bob 身份与候选原样保留（候选已满 3 次，本帧命中即稳定为 bob）。
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

    // 观测内部身份：一帧仍命中两框的**实际检测**（非空检测，理由见上）
    const auto views = tc.update({face(0, 0, 100, 150, true, "alice", 0.90f),
                                  face(200, 0, 100, 150, true, "bob", 0.91f)}, 1300, true);
    EXPECT_EQ(tc.trackCount(), 2);  // 轨迹本体保留，不丢框
    ASSERT_EQ(views.size(), 2u);
    std::vector<std::string> ids;
    for (const auto& v : views) {
        EXPECT_GT(v.bbox.area(), 0);  // bbox 保留
        ids.push_back(v.stableId);
    }
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], "Unknown");  // alice 身份被撤销（候选也从 0 重新累计）
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

// ── 阈值验收（第六轮 Codex 建议补的两类缺口）──────────────────────────────
//
// 缺口一：既有 CenterDistanceFallbackMatchesMovedFace 先用 **3 帧静止**建立已确认轨迹、之后才
// 移动，因此它证明的是「**已确认**轨迹在大位移下不丢」，**不能**证明「**持续移动**的目标能完成
// **首次**确认」。本用例补上：目标自首帧起每帧位移 79px（IoU≈0.12 强制走中心距离回退），
// 逐步中心偏移 79 ≤ kCenterMaxRatio*100 = 80 → 应连续命中并在第 3 帧确认、对外输出。
TEST(TrackCoordinator, ContinuouslyMovingTargetCompletesInitialConfirmation) {
    pipeline::TrackCoordinator tc;
    // 帧 1：首现即未确认轨迹（confirmFrames=1）
    EXPECT_TRUE(tc.update({face(0, 0, 100, 100)}, 1000, true).empty());
    // 帧 2：位移 79px → IoU 不足，中心偏移 79 ≤ 80 → 回退匹配
    EXPECT_TRUE(tc.update({face(79, 0, 100, 100)}, 1100, true).empty());
    // 帧 3：再位移 79px → 第 3 次命中，确认并输出
    const auto t3 = tc.update({face(158, 0, 100, 100)}, 1200, true);
    ASSERT_EQ(t3.size(), 1u) << "持续移动的目标应能完成首次确认（0.8 阈值内）";
    EXPECT_EQ(t3[0].trackId, 1);
    EXPECT_EQ(t3[0].bbox.x, 158);
}

// 缺口二：阈值**边界**的可执行界定。单帧位移 81px（中心偏移 81 > 0.8*100=80）时，IoU 与中心
// 距离回退**双双失配**，每帧新建轨迹且下一帧即因「未确认 + 一次实际缺失」被删，故**永无确认**、
// 对外恒不显示。这给出「0.8 在多大单帧位移下失效」的判据，供录像回放调参时对照。
// 注：这是**条件性结果**（取决于单帧位移与框尺寸之比），不是所有大位移场景的必然结局——
// 相邻用例已证明 79px/帧可持续确认。
TEST(TrackCoordinator, DisplacementBeyondFallbackRatioNeverConfirms) {
    pipeline::TrackCoordinator tc;
    for (int i = 0; i < 6; ++i) {
        EXPECT_TRUE(tc.update({face(i * 81, 0, 100, 100)}, 1000 + i * 100, true).empty())
                << "位移超出回退上限时不应有轨迹被确认（第 " << i << " 帧）";
    }
    // 逐帧新建 + 下一帧即删，故存续轨迹数不超过 1。
    EXPECT_LE(tc.trackCount(), 1) << "超出回退上限应无法复用同一条轨迹";
}

// ── 任务 C2：阈值提升为 TrackConfig 字段后的边界用例 ──────────────────────────
// 此前这些阈值（centerMaxRatio / sizeRatioMin / sizeRatioMax / ttlMs）在 TrackCoordinator.cpp 里
// 是**编译期常量、完全不可注入**，边界行为无法在测试里控制。现可注入，故补齐
// 「尺寸比上下界」「TTL 边界」与「可注入性」三组。

/**
 * 尺寸比上下界（sizeRatioMin/Max=0.5/2.0）。
 *
 * <p><b>构造要点</b>：必须让 IoU **先**不足（否则 IoU 路径直接命中、根本走不到回退）。
 * 用「大框完全包含小框」得到 IoU = 参照面积/检测面积：200×170 → 0.294、210×170 → 0.28，
 * 均 < matchIouThreshold(0.3)；同时令检测框与参照**同心**（中心距离 0），
 * 确保不会被中心距离判据先行拒绝——这样剩下的唯一变量就是尺寸比。
 */
TEST(TrackCoordinator, SizeRatioBoundsAreEnforced) {
    // 上界恰好 2.0：宽 200/100 = 2.0 ∈ 界内，高 170/100 = 1.7 ∈ 界内 → 回退匹配同一轨迹
    pipeline::TrackCoordinator atUpper;
    EXPECT_TRUE(atUpper.update({face(0, 0, 100, 100)}, 1000, true).empty());
    EXPECT_TRUE(atUpper.update({face(0, 0, 100, 100)}, 1100, true).empty());
    ASSERT_EQ(atUpper.update({face(0, 0, 100, 100)}, 1200, true).size(), 1u);
    const auto atBound = atUpper.update({face(-50, -35, 200, 170)}, 1300, true);
    ASSERT_EQ(atBound.size(), 1u);
    EXPECT_EQ(atBound[0].trackId, 1) << "恰好 2.0 应仍在界内";
    EXPECT_EQ(atUpper.trackCount(), 1) << "界内不应新建轨迹";

    // 上界超出：宽 210/100 = 2.1 > 2.0 → 回退拒绝 → **新建**轨迹
    // 注：被拒绝的检测不会并入旧轨迹，但旧轨迹（已确认 + 缺失 1 次）仍会输出，
    //     故判据是「轨迹数变为 2」，而不是「输出里的 trackId 变化」。
    pipeline::TrackCoordinator overUpper;
    EXPECT_TRUE(overUpper.update({face(0, 0, 100, 100)}, 1000, true).empty());
    EXPECT_TRUE(overUpper.update({face(0, 0, 100, 100)}, 1100, true).empty());
    ASSERT_EQ(overUpper.update({face(0, 0, 100, 100)}, 1200, true).size(), 1u);
    overUpper.update({face(-55, -35, 210, 170)}, 1300, true);
    EXPECT_EQ(overUpper.trackCount(), 2) << "超出尺寸比上界应新建轨迹（而非并入原轨迹）";

    // 下界超出：宽 40/100 = 0.4 < 0.5 → 同样拒绝
    pipeline::TrackCoordinator underLower;
    EXPECT_TRUE(underLower.update({face(0, 0, 100, 100)}, 1000, true).empty());
    EXPECT_TRUE(underLower.update({face(0, 0, 100, 100)}, 1100, true).empty());
    ASSERT_EQ(underLower.update({face(0, 0, 100, 100)}, 1200, true).size(), 1u);
    underLower.update({face(0, 0, 40, 40)}, 1300, true);
    EXPECT_EQ(underLower.trackCount(), 2) << "低于尺寸比下界应新建轨迹（而非并入原轨迹）";
}

/**
 * 中心距离比可注入：把 centerMaxRatio 收紧到 0.5 后，**同一段位移序列不再确认**。
 *
 * <p>这条同时证明两件事：① 阈值确实可在不改源码的前提下调整（C2 的目的）；② 默认值 0.8 下
 * 该序列能确认（见 ContinuouslyMovingTargetCompletesInitialConfirmation），故收紧确实改变了行为。
 */
TEST(TrackCoordinator, CenterMaxRatioIsInjectableAndChangesBehavior) {
    pipeline::TrackConfig tight{};
    tight.centerMaxRatio = 0.5f;   // 0.5*100=50 < 79 → 本序列不再匹配
    pipeline::TrackCoordinator tc(tight);

    EXPECT_TRUE(tc.update({face(0, 0, 100, 100)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(79, 0, 100, 100)}, 1100, true).empty());
    EXPECT_TRUE(tc.update({face(158, 0, 100, 100)}, 1200, true).empty())
            << "收紧中心距离比后，79px/帧不应再完成首次确认";
}

/** TTL 边界：跳过检测时超过 ttlMs 仍应兜底清理（此前 ttlMs 也不可注入）。 */
TEST(TrackCoordinator, TtlBoundaryIsEnforced) {
    pipeline::TrackConfig shortTtl{};
    shortTtl.ttlMs = 300;
    pipeline::TrackCoordinator tc(shortTtl);

    EXPECT_TRUE(tc.update({face(0, 0, 100, 100)}, 1000, true).empty());
    EXPECT_TRUE(tc.update({face(0, 0, 100, 100)}, 1100, true).empty());
    ASSERT_EQ(tc.update({face(0, 0, 100, 100)}, 1200, true).size(), 1u);

    // 恰好等于 TTL：仍保留（判据是 <=）
    EXPECT_EQ(tc.update({}, 1500, false).size(), 1u) << "恰在 TTL 内不应清理";
    // 超过 TTL 1ms：兜底清理
    EXPECT_TRUE(tc.update({}, 1501, false).empty()) << "超过 TTL 应兜底清理已确认轨迹";
}

// ── 结构式守卫（读源码文本）：引擎入口的跨文件不变量 ────────────────────────────
//
// ⚠️ **为什么放在本文件**：读源码文本需要 `RK_TEST_SOURCE_ROOT`；该宏现对 `core_unit_tests`、
//    `core_gtest_tests`、`win_unit_tests` 三个 target 定义（分别见 cmake/core.cmake、CMakeLists.txt
//    与 cmake/windows.cmake 的注入块）。本文件（test_track_coordinator.cpp）正是 core_unit_tests 的源之一。
//    范式与 test_event_manager.cpp 的「生产头文件不得依赖 gtest」守卫一致：**先剥离注释**再匹配，
//    故源码中讨论这些符号的说明性注释不会造成误报。
//
// ⚠️ **判别力的边界（如实登记，勿读成强保证）**：本段挡的是「**直接删掉**守卫/改写调用点」这类
//    本仓历史上真实发生过的改动；对「换个写法绕过」（改函数名、把守卫挪进辅助函数、用宏包一层）
//    无效——那类绕过需要编译期/运行期机制才能拦。本段与仓内既有结构测试（test_checked_arith.cpp、
//    test_event_manager.cpp）同一量级，**不**主张更强。
#ifdef RK_TEST_SOURCE_ROOT
namespace {

// 读取源码根下相对路径文件；失败（或路径写错）返回空串——调用方**必须**显式断言非空，
// 否则「扫描集合为空」会静默地让所有断言都通过（空 == 空）。
std::string readRepoFile(const std::string& rel) {
    std::ifstream in(std::string(RK_TEST_SOURCE_ROOT) + "/" + rel, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// 剥离 `//` 行注释与 `/* */` 块注释（块注释状态**跨行**保留），与 test_event_manager.cpp 同口径。
std::string stripComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool inBlock = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (inBlock) {
            if (src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/') {
                inBlock = false;
                ++i;
            }
            continue;
        }
        if (src[i] == '/' && i + 1 < src.size()) {
            if (src[i + 1] == '/') {  // 行注释：跳到行尾（保留换行，供按行判据使用）
                while (i < src.size() && src[i] != '\n') ++i;
                out.push_back('\n');
                continue;
            }
            if (src[i + 1] == '*') {
                inBlock = true;
                ++i;
                continue;
            }
        }
        out.push_back(src[i]);
    }
    return out;
}

// 去掉全部空白：使断言对格式漂移免疫（`if ( guard ( ... ) )` 与 `if(guard(...))` 等价）。
std::string squeeze(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return out;
}

// 取 `open`（指向 '(' 之后一个字符处）开始、与深度 1 匹配的实参文本；返回该 ')' 的下一个下标。
// 深度感知是为容忍实参里出现括号（如 boxOf(...)）时不被第一个 ')' 截断。
std::size_t argListEnd(const std::string& s, std::size_t open) {
    int depth = 1;
    std::size_t i = open;
    for (; i < s.size() && depth > 0; ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') --depth;
    }
    return i;  // 指向匹配 ')' 之后
}

// 取 `open`（指向 '{'）所属**块**的收尾 `}` 之后一个下标；未配对到收尾时返回 std::string::npos。
// 用途：把结构式断言的切片限制在**单个函数体**内，避免「切片/窗口越界读到下一个函数的定义」——
// 那会让「删掉本函数的守卫」被邻函数的守卫掩盖而**假通过**（FACE-PIPE-015 用例曾如此）。
// ⚠️ 已知脆弱面（如实登记）：这是**纯字符计数**，源码里的字面量 '{' / '}'（字符串或字符常量）会
//    使其失准——多计 ⇒ 收尾被判在更远处（切片变大，最坏退回旧行为）；少计 ⇒ 切片**早截**（可能造成
//    假红，即目标文本被截断在切片之外）。Engine.cpp 现行文本已实测均衡（全文件花括号净深度 0，且
//    本文件两处调用点的切片终点都正落在各自函数收尾 `}` 上）。
std::size_t matchBrace(const std::string& s, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        if (s[i] == '{') {
            ++depth;
        } else if (s[i] == '}') {
            --depth;
            if (depth == 0) return i + 1;  // 指向匹配 '}' 之后
        }
    }
    return std::string::npos;  // 未配对（计数失准或源码被截断）
}

}  // namespace

// FACE-PIPE-015：引擎的 5 个「会取 tracksMu_ 的公开入口」必须在**函数体首部**调用
// reenteredFromIdentityEmitGate，否则免死锁契约退化为注释——该函数是 thread_local 标记
// `t_insideIdentityEmitGate` 的唯一读取点，任一入口缺失它，该入口在结果回调（已持
// identityEmitMu_）内同步重入时就不再被拒绝，与撤销路径构成 ABBA 死锁
//（见 Engine.h 的 identityEmitMu_ 条目；穷尽集为 run / setRealtimePipeline / applyGalleryUpdate /
//  getTrackViews / processFrameOnce）。
//
// 判别点：删掉上述任一函数体首部的那行 `if (reenteredFromIdentityEmitGate(__func__)) ...`
//        ⇒ 本用例翻红（该入口的**函数体首部窗口**内不再出现该符号）。
// ⚠️ 窗口是「从函数体 `{` 起、最多 400 字符，且**不越过本函数体**」——后者按 `matchBrace` 取函数体
//    边界实现。**这一边界是必需的**：5 个入口里有几个函数体极短（getTrackViews 的函数体只有百余
//    字符），固定 400 字符窗口会越界读到**下一个函数**（如 processFrameOnce）的守卫 ⇒ 删掉本入口的
//    守卫仍假通过。修正后 5 个入口**一视同仁**：窗口内出现的守卫必然来自本函数体，故删掉任一个都必红。
//    窗口上限仍为 400 是**有意放宽**：容忍「守卫之前插入一条普通语句」的格式变化，但不允许守卫被挪到
//    函数中后段（那正是本契约失效的形态）。
// ⚠️ 脆弱面（如实登记）：① 400 字符是**经验上限**——若某入口在守卫之前插入超过 400 字符的普通语句，
//    会假红；② `matchBrace` 是纯字符计数，函数体内若出现字面量 '{'/'}'（字符串/字符常量）会导致
//    切片失准：多计 ⇒ 窗口退回固定的 400 字符（仍可能越界），少计 ⇒ 假红（守卫被截断在窗口外）。
// ⚠️ `run()` **只**在此处有覆盖，没有行为用例：守卫缺失时它会与已持 identityEmitMu_ 的钩子
//    自锁挂死，失败形态不可诊断（挂死也是红，但排查成本高），故刻意不写——详见 test_engine.cpp
//    中 FACE-PIPE-015 用例的头注释。
TEST(EngineStructuralGuard, AllTracksMuEntryPointsCallEmitGateReentryGuard) {
    const std::string raw = readRepoFile("src/cpp/src/Engine.cpp");
    ASSERT_FALSE(raw.empty()) << "无法读取 src/cpp/src/Engine.cpp（RK_TEST_SOURCE_ROOT="
                              << RK_TEST_SOURCE_ROOT << "）";
    const std::string body = squeeze(stripComments(raw));
    ASSERT_GT(body.size(), 10000u) << "Engine.cpp 文本异常短，疑似路径写错";

    const char* const kEntries[] = {
        "Engine::run(", "Engine::setRealtimePipeline(", "Engine::applyGalleryUpdate(",
        "Engine::getTrackViews(", "Engine::processFrameOnce("};
    ASSERT_EQ(sizeof(kEntries) / sizeof(kEntries[0]), 5u)
            << "入口穷尽集为 5 个（见 Engine.h 的 identityEmitMu_ 条目）；集合变了须同步本用例";

    for (const char* entry : kEntries) {
        const std::size_t at = body.find(entry);
        ASSERT_NE(at, std::string::npos) << "Engine.cpp 中找不到入口定义: " << entry;
        // 这 5 个入口的形参表内均无 `{`（含模板实参 <> 与引用 &），故首个 `{` 即函数体起始。
        const std::size_t brace = body.find('{', at);
        ASSERT_NE(brace, std::string::npos) << "找不到函数体起始: " << entry;
        // 首部窗口（去空白后守卫调用本身约 40 字符）：上限 400 只为容忍「守卫之前插入一条普通
        // 语句」的格式变化；**实际下界由本函数体的收尾 `}` 决定**（未配对到收尾时 limit 取文件末，
        // 但 window 仍被 400 上限截住 ⇒ 退回旧的「固定 400 字符窗口」行为）。
        constexpr std::size_t kPrologueChars = 400;
        const std::size_t bodyEnd = matchBrace(body, brace);
        const std::size_t limit = (bodyEnd == std::string::npos) ? body.size() : bodyEnd;
        const std::size_t window = std::min(kPrologueChars, limit - brace);
        EXPECT_NE(body.substr(brace, window).find("reenteredFromIdentityEmitGate("),
                  std::string::npos)
                << entry << " 的函数体首部（窗口不越过函数体边界）未见 reenteredFromIdentityEmitGate"
                   " 调用：该入口的 FACE-PIPE-015 重入拒绝已失效（删掉那行即触发本失败）";
    }
}

// FACE-TRACK-002：`trackFaces` 的 `detectionRan` 实参白名单。
//
// 为什么需要：生产侧该参数**恒为 true**——唯一来源是 runRealtimeDetect 的成功出口（含 boxes 为空的
// **实际空检测**），兼容路径显式传 true；模型/管线不可用时根本不调用 trackFaces。由此
// TrackCoordinator 的 `if (!detectionRan) return views();` 早退分支在生产路径上**不可达**，
// 「实际空检测 → 连续缺失 2 次删框」的消失语义才成立。
//
// 白名单**只放两种形态**（其余一律视为可疑）：
//   · 字面 `true`            —— 兼容路径显式声明「本次确实执行了检测」；
//   · 标识符 `detectionRan`  —— 实时路径把 runRealtimeDetect 的判定原样透传。
// 不写死调用点**数目**（新增合法调用点不应造成假红），但要求至少扫到 1 处，
// 使「扫描为空 == 全部合规」这种空转不可能通过。
//
// 判别点：把任一调用点的第 2 实参换成别的表达式（例如把 `detThrottleMode_` 的判定接进来）
//        ⇒ 本用例翻红。行为侧对应覆盖见 test_engine.cpp 的
//        DetectionThrottleModeDoesNotSuppressRealDetection。
// ⚠️ 脆弱面（如实登记，勿当作强保证）：跳过**定义体**靠的是**硬编码前缀** `"voidEngine::"`
//    （去空白后的 `void Engine::trackFaces(`）。一旦 `trackFaces` 的返回类型改成
//    非 `void`（如 `bool`/`auto`）或改用别的限定写法（如 `Engine::trackFaces` 前加 inline/宏），
//    该前缀不再匹配 ⇒ **定义体被当作调用点**：其第 2 实参是形参声明（如 `booldetectionRan`），
//    于是 `EXPECT_TRUE(... == "true" || == "detectionRan")` **假红**。
//    「调用点确为 2 处」这一结论**不受影响**：它由源码实测（3 处 `trackFaces(` − 1 处定义）得出，
//    与运行期的前缀判别无关；受影响的只是本用例「是否误把定义当调用点」。
TEST(EngineStructuralGuard, TrackFacesDetectionRanArgumentStaysTriviallyTrue) {
    const std::string raw = readRepoFile("src/cpp/src/Engine.cpp");
    ASSERT_FALSE(raw.empty()) << "无法读取 src/cpp/src/Engine.cpp";
    const std::string body = squeeze(stripComments(raw));

    const char* const kCall = "trackFaces(";
    // ⚠️ 与返回类型强耦合（见上方脆弱面登记）：返回类型一变，这里就认不出定义体。
    const std::size_t kDefPrefixLen = std::strlen("voidEngine::");  // 定义形如 `voidEngine::trackFaces(`
    int callSites = 0;
    std::size_t pos = 0;
    while ((pos = body.find(kCall, pos)) != std::string::npos) {
        const std::size_t open = pos + std::strlen(kCall);
        const std::size_t end = argListEnd(body, open);
        if (pos >= kDefPrefixLen &&
            body.compare(pos - kDefPrefixLen, kDefPrefixLen, "voidEngine::") == 0) {
            pos = end;  // 定义体本身，只检查调用点
            continue;
        }
        const std::string args = body.substr(open, end - 1 - open);  // end-1 指向匹配的 ')'
        // 按深度 0 的 ',' 切分实参
        std::vector<std::string> parts;
        std::size_t segStart = 0;
        int depth = 0;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == '(') ++depth;
            else if (args[i] == ')') --depth;
            else if (args[i] == ',' && depth == 0) {
                parts.push_back(args.substr(segStart, i - segStart));
                segStart = i + 1;
            }
        }
        parts.push_back(args.substr(segStart));
        ASSERT_GE(parts.size(), 2u) << "trackFaces 调用点实参不足 2 个: " << args;
        const std::string& detectionRanArg = parts[1];
        EXPECT_TRUE(detectionRanArg == "true" || detectionRanArg == "detectionRan")
                << "trackFaces 第 2 实参只允许字面 true 或标识符 detectionRan，实得: "
                << detectionRanArg
                << "（把节流判定接到此处会让「跳过检测」在生产可达，改变旧框消失语义）";
        ++callSites;
        pos = end;
    }
    EXPECT_GE(callSites, 1) << "未在 Engine.cpp 中扫到任何 trackFaces 调用点（符号改名或路径变化？）";
}

// FACE-TRACK-002：检测节流模式**不得**接入检测执行路径。
//
// 上面那条「恒传 true」不变量还有一半前提未被任何断言覆盖：**没有任何生产代码把节流判定接到检测
// 门控上**。本用例把该前提写成可执行约束——在 runRealtimeDetect 与 processFrameRealtime 的函数体
// 内不得出现 `detThrottleMode_`（它的合法触点只有配置写入 updateDetectionThrottle /
// updateInferenceThrottle 与状态展示 getDetectionThrottleMode）。
//
// 判别点：在 runRealtimeDetect 内加节流门（例如节流窗内 `detectionRan = false; return true;`）
//        ⇒ 本用例翻红；行为侧对应覆盖见 test_engine.cpp 的
//        DetectionThrottleModeDoesNotSuppressRealDetection。
// ⚠️ 脆弱性（如实登记）：这是**白名单式**结构断言——将来若合法地在这两个函数体内读一次
//    `detThrottleMode_`（例如加一行诊断日志），本用例会假红，届时须**连同行为用例一起**复核是否
//    真的接线了节流语义，再调整白名单，而不是直接删断言。
// ⚠️ 切片边界（已加固，但仍是字符级近似）：切片终点原为 `code.find("\n}\n", brace)`，依赖「顶层
//    函数收尾 `}` 独占一行」这一 Allman 风格；内层块若把闭合 `}` 顶格写，切片会**早截**（这两个
//    函数体当前各约 440 / 约 1110 字符，早截后仍 >200 故 `ASSERT_GT` 放行），此后在**真**函数体内读
//    `detThrottleMode_` 就不再翻红（假通过）。现改为 `matchBrace` 花括号配对计数，不再依赖缩进风格；
//    代价是该计数为**纯字符计数**，函数体内出现字面量 '{'/'}' 会失准（失准 ⇒ 断言失败，属假红而非
//    假通过，见 matchBrace 的注记）。
TEST(EngineStructuralGuard, DetectionThrottleModeIsNotWiredIntoDetectPath) {
    const std::string raw = readRepoFile("src/cpp/src/Engine.cpp");
    ASSERT_FALSE(raw.empty()) << "无法读取 src/cpp/src/Engine.cpp";
    const std::string code = stripComments(raw);  // 保留原始换行（切片边界见下方 matchBrace 说明）

    const char* const kFns[] = {"Engine::runRealtimeDetect(", "Engine::processFrameRealtime("};
    for (const char* fn : kFns) {
        const std::size_t at = code.find(fn);
        ASSERT_NE(at, std::string::npos) << "Engine.cpp 中找不到函数定义: " << fn;
        const std::size_t brace = code.find('{', at);
        ASSERT_NE(brace, std::string::npos) << "找不到函数体起始: " << fn;
        // 按**花括号配对**取整个函数体（不再用 `\n}\n` 那类依赖缩进风格的锚点，见上方脆弱性登记）。
        const std::size_t end = matchBrace(code, brace);
        ASSERT_NE(end, std::string::npos)
                << fn << " 的函数体花括号未配对到收尾（格式变化或计数失准？）: " << fn;
        const std::string fnBody = code.substr(brace, end - brace);
        ASSERT_GT(fnBody.size(), 200u)
                << fn << " 的切片过短（切片判据可能已失效，勿让空切片伪装成通过）";
        EXPECT_EQ(fnBody.find("detThrottleMode_"), std::string::npos)
                << fn << " 内出现 detThrottleMode_：检测门控已被节流模式接线，"
                   "「生产恒传 detectionRan=true」不变量失效";
    }
}

#else
// 这条 `#error` 是**故意**的（同 test_event_manager.cpp 的范式）：若本文件被编进一个未定义
// `RK_TEST_SOURCE_ROOT` 的 target，上方整段结构式守卫会**静默消失**（既不注册、也不 skip、也不
// fail，零信号）。宁可让构建当场失败，也不要留一个看起来在守、实际不存在的守卫。
// 当前唯一编入本文件的 target 是 core_unit_tests，它定义了该宏（cmake/core.cmake）。
#error "test_track_coordinator.cpp 的结构式守卫依赖 RK_TEST_SOURCE_ROOT；缺失它会让守卫静默消失"
#endif  // RK_TEST_SOURCE_ROOT
