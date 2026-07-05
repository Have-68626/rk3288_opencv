#include "ThresholdPolicy.h"
#include "InferenceThrottle.h"
#include <gtest/gtest.h>

#include <string>

TEST(ThresholdPolicy, VersionAndConsecutive) {
    ThresholdPolicyVersion v1;
    v1.versionId = "v1";
    v1.acceptThreshold = 0.80f;
    v1.consecutivePassesToTrigger = 3;

    ThresholdDecisionPolicy p(v1);

    auto r1 = p.feed(0.90f, true);
    EXPECT_TRUE(r1.passNow);
    EXPECT_FALSE(r1.triggeredNow);
    EXPECT_EQ(r1.passStreak, 1);

    auto r2 = p.feed(0.85f, true);
    EXPECT_TRUE(r2.passNow);
    EXPECT_FALSE(r2.triggeredNow);
    EXPECT_EQ(r2.passStreak, 2);

    auto r3 = p.feed(0.81f, true);
    EXPECT_TRUE(r3.passNow);
    EXPECT_TRUE(r3.triggeredNow);
    EXPECT_TRUE(r3.triggeredLatched);
    EXPECT_EQ(r3.passStreak, 3);

    auto r4 = p.feed(0.95f, true);
    EXPECT_TRUE(r4.passNow);
    EXPECT_FALSE(r4.triggeredNow);
    EXPECT_TRUE(r4.triggeredLatched);
    EXPECT_EQ(r4.passStreak, 4);

    auto r5 = p.feed(0.10f, true);
    EXPECT_FALSE(r5.passNow);
    EXPECT_FALSE(r5.triggeredNow);
    EXPECT_FALSE(r5.triggeredLatched);
    EXPECT_EQ(r5.passStreak, 0);

    ThresholdPolicyVersion v2;
    v2.versionId = "v2";
    v2.acceptThreshold = 0.50f;
    v2.consecutivePassesToTrigger = 2;

    std::string err;
    ASSERT_TRUE(p.apply(v2, err));
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(p.active().versionId, "v2");
    EXPECT_EQ(p.active().acceptThreshold, 0.50f);
    EXPECT_EQ(p.active().consecutivePassesToTrigger, 2);

    auto r6 = p.feed(0.60f, true);
    EXPECT_TRUE(r6.passNow);
    EXPECT_FALSE(r6.triggeredNow);
    EXPECT_EQ(r6.passStreak, 1);

    auto r7 = p.feed(0.60f, true);
    EXPECT_TRUE(r7.passNow);
    EXPECT_TRUE(r7.triggeredNow);
    EXPECT_EQ(r7.passStreak, 2);

    ASSERT_TRUE(p.rollbackPrevious(err));
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(p.active().versionId, "v1");

    auto r8 = p.feed(0.79f, true);
    EXPECT_FALSE(r8.passNow);
    EXPECT_EQ(r8.passStreak, 0);
}

TEST(ThresholdPolicy, RollbackEmptyHistory) {
    ThresholdPolicyVersion v1;
    v1.versionId = "v1";
    v1.acceptThreshold = 0.80f;
    v1.consecutivePassesToTrigger = 3;

    ThresholdDecisionPolicy p(v1);

    std::string err;
    bool success = p.rollbackPrevious(err);

    EXPECT_FALSE(success);
    EXPECT_NE(err.find("无可回滚版本"), std::string::npos);
}

TEST(InferenceThrottle, ParseAndClamp) {
    EXPECT_EQ(parseInferenceThrottleMode("off"), InferenceThrottleMode::Off);
    EXPECT_EQ(parseInferenceThrottleMode("AUTO"), InferenceThrottleMode::Auto);
    EXPECT_EQ(parseInferenceThrottleMode("manual"), InferenceThrottleMode::Manual);
    EXPECT_EQ(parseInferenceThrottleMode("x"), InferenceThrottleMode::Off);

    EXPECT_EQ(clampInferenceIntervalMs(0), kInferenceIntervalMinMs);
    EXPECT_EQ(clampInferenceIntervalMs(9999), kInferenceIntervalMaxMs);
    EXPECT_EQ(clampInferenceIntervalMs(150), 150);
}
