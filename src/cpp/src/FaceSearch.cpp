#include "FaceSearch.h"

#include <algorithm>
#include <cmath>

namespace {

// Optimized to use float instead of double to maximize SIMD vectorization
// and improve execution speed on the RK3288 (ARM Cortex-A17) platform.
static float l2Norm(const float* v, std::size_t dim) {
    float s = 0.0f;
    for (std::size_t i = 0; i < dim; i++) {
        const float x = v[i];
        s += x * x;
    }
    const float n = std::sqrt(s);
    if (!(n > 0.0f)) return 0.0f;
    return n;
}

// Optimized to use float instead of double to maximize SIMD vectorization
// and improve execution speed on the RK3288 (ARM Cortex-A17) platform.
static float cosineSimilarity(const float* a, float aNorm, const float* b, float bNorm, std::size_t dim, bool assumeL2Normalized) {
    float dot = 0.0f;
    for (std::size_t i = 0; i < dim; i++) {
        dot += a[i] * b[i];
    }
    if (assumeL2Normalized) return dot;
    const float denom = aNorm * bNorm;
    if (!(denom > 0.0f)) return -1.0f;
    return dot / denom;
}

}  // namespace

bool FaceSearchLinearIndex::reset(std::vector<FaceSearchEntry> entries, std::size_t dim, std::string& err) {
    err.clear();
    if (dim == 0) {
        err = "FaceSearchLinearIndex: dim 不能为 0";
        return false;
    }
    if (entries.size() > 10000) {
        err = "FaceSearchLinearIndex: N 超过 10000（当前仅支持线性检索）";
        return false;
    }

    norms_.clear();
    norms_.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); i++) {
        const auto& e = entries[i];
        if (e.id.empty()) {
            err = "FaceSearchLinearIndex: 存在空 id";
            return false;
        }
        if (e.embedding.size() != dim) {
            err = "FaceSearchLinearIndex: embedding dim 不一致";
            return false;
        }
        norms_.push_back(l2Norm(e.embedding.data(), dim));
    }

    dim_ = dim;
    entries_ = std::move(entries);
    return true;
}

std::vector<FaceSearchHit> FaceSearchLinearIndex::searchTopK(const std::vector<float>& query, std::size_t topK, const FaceSearchOptions& opt, std::string& err) const {
    err.clear();
    if (dim_ == 0) {
        err = "FaceSearchLinearIndex: index 未初始化";
        return {};
    }
    if (query.size() != dim_) {
        err = "FaceSearchLinearIndex: query dim 不匹配";
        return {};
    }
    if (topK == 0 || entries_.empty()) return {};
    if (topK > entries_.size()) topK = entries_.size();

    const float qn = opt.assumeL2Normalized ? 1.0f : l2Norm(query.data(), dim_);

    std::vector<FaceSearchHit> hits;
    hits.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); i++) {
        const auto& e = entries_[i];
        const float score = cosineSimilarity(query.data(), qn, e.embedding.data(), norms_[i], dim_, opt.assumeL2Normalized);
        if (std::isnan(score)) continue;
        hits.push_back(FaceSearchHit{e.id, i, score});
    }

    const float eps = opt.tieEpsilon >= 0.0f ? opt.tieEpsilon : 0.0f;
    auto comp = [&](const FaceSearchHit& a, const FaceSearchHit& b) {
        const float da = a.score;
        const float db = b.score;
        const float diff = da - db;
        if (diff > eps) return true;
        if (-diff > eps) return false;
        return a.index < b.index;
    };

    if (hits.size() > topK) {
        std::partial_sort(hits.begin(), hits.begin() + topK, hits.end(), comp);
        hits.resize(topK);
    } else {
        std::sort(hits.begin(), hits.end(), comp);
    }
    return hits;
}

