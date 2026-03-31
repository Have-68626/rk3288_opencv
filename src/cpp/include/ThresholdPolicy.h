#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct ThresholdPolicyVersion {
    std::string versionId;
    float acceptThreshold = 0.5f;
    std::size_t consecutivePassesToTrigger = 1;
};

struct ThresholdDecisionResult {
    bool passNow = false;
    bool triggeredNow = false;
    bool triggeredLatched = false;
    std::size_t passStreak = 0;
    float score = -1.0f;
    float threshold = 0.0f;
    std::string versionId;
};

class ThresholdDecisionPolicy {
public:
    explicit ThresholdDecisionPolicy(ThresholdPolicyVersion initial);

    const ThresholdPolicyVersion& active() const;
    std::vector<ThresholdPolicyVersion> history() const;

    bool apply(ThresholdPolicyVersion next, std::string& err);
    bool rollbackPrevious(std::string& err);
    bool rollbackTo(const std::string& versionId, std::string& err);

    void reset();
    ThresholdDecisionResult feed(float bestScore, bool hasCandidate);

private:
    bool validate(const ThresholdPolicyVersion& v, std::string& err) const;
    void resetState();

    std::vector<ThresholdPolicyVersion> history_;
    std::size_t passStreak_ = 0;
    bool triggeredLatched_ = false;
};

