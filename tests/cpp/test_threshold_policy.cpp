#include "ThresholdPolicy.h"

#include <string>

bool test_threshold_policy_version_and_consecutive() {
    ThresholdPolicyVersion v1;
    v1.versionId = "v1";
    v1.acceptThreshold = 0.80f;
    v1.consecutivePassesToTrigger = 3;

    ThresholdDecisionPolicy p(v1);

    auto r1 = p.feed(0.90f, true);
    if (!r1.passNow) return false;
    if (r1.triggeredNow) return false;
    if (r1.passStreak != 1) return false;

    auto r2 = p.feed(0.85f, true);
    if (!r2.passNow) return false;
    if (r2.triggeredNow) return false;
    if (r2.passStreak != 2) return false;

    auto r3 = p.feed(0.81f, true);
    if (!r3.passNow) return false;
    if (!r3.triggeredNow) return false;
    if (!r3.triggeredLatched) return false;
    if (r3.passStreak != 3) return false;

    auto r4 = p.feed(0.95f, true);
    if (!r4.passNow) return false;
    if (r4.triggeredNow) return false;
    if (!r4.triggeredLatched) return false;
    if (r4.passStreak != 4) return false;

    auto r5 = p.feed(0.10f, true);
    if (r5.passNow) return false;
    if (r5.triggeredNow) return false;
    if (r5.triggeredLatched) return false;
    if (r5.passStreak != 0) return false;

    ThresholdPolicyVersion v2;
    v2.versionId = "v2";
    v2.acceptThreshold = 0.50f;
    v2.consecutivePassesToTrigger = 2;

    std::string err;
    if (!p.apply(v2, err)) return false;
    if (!err.empty()) return false;
    if (p.active().versionId != "v2") return false;
    if (p.active().acceptThreshold != 0.50f) return false;
    if (p.active().consecutivePassesToTrigger != 2) return false;

    auto r6 = p.feed(0.60f, true);
    if (!r6.passNow) return false;
    if (r6.triggeredNow) return false;
    if (r6.passStreak != 1) return false;

    auto r7 = p.feed(0.60f, true);
    if (!r7.passNow) return false;
    if (!r7.triggeredNow) return false;
    if (r7.passStreak != 2) return false;

    if (!p.rollbackPrevious(err)) return false;
    if (!err.empty()) return false;
    if (p.active().versionId != "v1") return false;

    auto r8 = p.feed(0.79f, true);
    if (r8.passNow) return false;
    if (r8.passStreak != 0) return false;

    return true;
}

bool test_threshold_policy_rollback_empty_history() {
    ThresholdPolicyVersion v1;
    v1.versionId = "v1";
    v1.acceptThreshold = 0.80f;
    v1.consecutivePassesToTrigger = 3;

    ThresholdDecisionPolicy p(v1);

    std::string err;
    bool success = p.rollbackPrevious(err);

    // Verify it fails
    if (success) return false;

    // Verify error message contains expected substring
    if (err.find("无可回滚版本") == std::string::npos) return false;

    return true;
}
