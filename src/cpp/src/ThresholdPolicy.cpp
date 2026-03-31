#include "ThresholdPolicy.h"

#include <algorithm>

ThresholdDecisionPolicy::ThresholdDecisionPolicy(ThresholdPolicyVersion initial) {
    std::string err;
    if (!validate(initial, err)) {
        initial.versionId = initial.versionId.empty() ? "invalid" : initial.versionId;
        initial.acceptThreshold = 0.5f;
        initial.consecutivePassesToTrigger = 1;
    }
    history_.push_back(std::move(initial));
    resetState();
}

const ThresholdPolicyVersion& ThresholdDecisionPolicy::active() const {
    return history_.back();
}

std::vector<ThresholdPolicyVersion> ThresholdDecisionPolicy::history() const {
    return history_;
}

bool ThresholdDecisionPolicy::validate(const ThresholdPolicyVersion& v, std::string& err) const {
    err.clear();
    if (v.versionId.empty()) {
        err = "ThresholdDecisionPolicy: versionId 不能为空";
        return false;
    }
    if (!(v.acceptThreshold >= -1.0f && v.acceptThreshold <= 1.0f)) {
        err = "ThresholdDecisionPolicy: acceptThreshold 超出 [-1,1]";
        return false;
    }
    if (v.consecutivePassesToTrigger == 0) {
        err = "ThresholdDecisionPolicy: consecutivePassesToTrigger 不能为 0";
        return false;
    }
    return true;
}

bool ThresholdDecisionPolicy::apply(ThresholdPolicyVersion next, std::string& err) {
    if (!validate(next, err)) return false;
    history_.push_back(std::move(next));
    resetState();
    return true;
}

bool ThresholdDecisionPolicy::rollbackPrevious(std::string& err) {
    err.clear();
    if (history_.size() <= 1) {
        err = "ThresholdDecisionPolicy: 无可回滚版本";
        return false;
    }
    history_.pop_back();
    resetState();
    return true;
}

bool ThresholdDecisionPolicy::rollbackTo(const std::string& versionId, std::string& err) {
    err.clear();
    if (versionId.empty()) {
        err = "ThresholdDecisionPolicy: versionId 不能为空";
        return false;
    }
    auto it = std::find_if(history_.begin(), history_.end(), [&](const ThresholdPolicyVersion& v) { return v.versionId == versionId; });
    if (it == history_.end()) {
        err = "ThresholdDecisionPolicy: 未找到目标版本";
        return false;
    }
    history_.erase(it + 1, history_.end());
    resetState();
    return true;
}

void ThresholdDecisionPolicy::resetState() {
    passStreak_ = 0;
    triggeredLatched_ = false;
}

void ThresholdDecisionPolicy::reset() {
    resetState();
}

ThresholdDecisionResult ThresholdDecisionPolicy::feed(float bestScore, bool hasCandidate) {
    const auto& v = active();
    const bool passNow = hasCandidate && bestScore >= v.acceptThreshold;

    bool triggeredNow = false;
    if (!passNow) {
        passStreak_ = 0;
        triggeredLatched_ = false;
    } else {
        passStreak_++;
        if (!triggeredLatched_ && passStreak_ >= v.consecutivePassesToTrigger) {
            triggeredLatched_ = true;
            triggeredNow = true;
        }
    }

    ThresholdDecisionResult r;
    r.passNow = passNow;
    r.triggeredNow = triggeredNow;
    r.triggeredLatched = triggeredLatched_;
    r.passStreak = passStreak_;
    r.score = bestScore;
    r.threshold = v.acceptThreshold;
    r.versionId = v.versionId;
    return r;
}

