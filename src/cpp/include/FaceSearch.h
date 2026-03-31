#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct FaceSearchEntry {
    std::string id;
    std::vector<float> embedding;
};

struct FaceSearchHit {
    std::string id;
    std::size_t index = 0;
    float score = -1.0f;
};

struct FaceSearchOptions {
    float tieEpsilon = 1e-6f;
    bool assumeL2Normalized = true;
};

class FaceSearchLinearIndex {
public:
    FaceSearchLinearIndex() = default;

    bool reset(std::vector<FaceSearchEntry> entries, std::size_t dim, std::string& err);

    std::size_t dim() const { return dim_; }
    std::size_t size() const { return entries_.size(); }
    const std::vector<FaceSearchEntry>& entries() const { return entries_; }

    std::vector<FaceSearchHit> searchTopK(const std::vector<float>& query, std::size_t topK, const FaceSearchOptions& opt, std::string& err) const;

private:
    std::size_t dim_ = 0;
    std::vector<FaceSearchEntry> entries_;
    std::vector<float> norms_;
};

