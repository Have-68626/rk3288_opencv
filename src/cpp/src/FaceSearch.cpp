#include "FaceSearch.h"

#include <algorithm>
#include <cmath>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace {

static float dotProductScalar(const float* a, const float* b, std::size_t dim) {
    float dot = 0.0f;
    for (std::size_t i = 0; i < dim; i++) {
        dot += a[i] * b[i];
    }
    return dot;
}

static float l2NormScalar(const float* v, std::size_t dim) {
    float s = 0.0f;
    for (std::size_t i = 0; i < dim; i++) {
        const float x = v[i];
        s += x * x;
    }
    const float n = std::sqrt(s);
    if (!(n > 0.0f)) return 0.0f;
    return n;
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
static float reduceAdd(float32x4_t v) {
    float32x2_t sum = vadd_f32(vget_low_f32(v), vget_high_f32(v));
    sum = vpadd_f32(sum, sum);
    return vget_lane_f32(sum, 0);
}

static float dotProductNeon(const float* a, const float* b, std::size_t dim) {
    // Use aligned loads when possible; vld1q_f32 on ARMv7 handles unaligned addresses
    // but some older ARM implementations may trap.  Check alignment and fall back
    // to scalar for sub-16-byte-aligned pointers.
    constexpr std::size_t kAlign = 16;
    const bool aligned = (reinterpret_cast<std::uintptr_t>(a) & (kAlign - 1)) == 0 &&
                         (reinterpret_cast<std::uintptr_t>(b) & (kAlign - 1)) == 0;
    if (!aligned) return dotProductScalar(a, b, dim);

    std::size_t i = 0;
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (; i + 4 <= dim; i += 4) {
        acc = vmlaq_f32(acc, vld1q_f32(a + i), vld1q_f32(b + i));
    }
    float dot = reduceAdd(acc);
    for (; i < dim; i++) {
        dot += a[i] * b[i];
    }
    return dot;
}

static float l2NormNeon(const float* v, std::size_t dim) {
    constexpr std::size_t kAlign = 16;
    const bool aligned = (reinterpret_cast<std::uintptr_t>(v) & (kAlign - 1)) == 0;
    if (!aligned) return l2NormScalar(v, dim);

    std::size_t i = 0;
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (; i + 4 <= dim; i += 4) {
        const float32x4_t x = vld1q_f32(v + i);
        acc = vmlaq_f32(acc, x, x);
    }
    float s = reduceAdd(acc);
    for (; i < dim; i++) {
        const float x = v[i];
        s += x * x;
    }
    const float n = std::sqrt(s);
    if (!(n > 0.0f)) return 0.0f;
    return n;
}
#endif

static float dotProduct(const float* a, const float* b, std::size_t dim) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    return dotProductNeon(a, b, dim);
#else
    return dotProductScalar(a, b, dim);
#endif
}

static float l2Norm(const float* v, std::size_t dim) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    return l2NormNeon(v, dim);
#else
    return l2NormScalar(v, dim);
#endif
}

static float cosineSimilarity(const float* a, float aNorm, const float* b, float bNorm, std::size_t dim, bool assumeL2Normalized) {
    if (assumeL2Normalized) return dotProduct(a, b, dim);
    const float denom = aNorm * bNorm;
    if (!(denom > 0.0f)) return -1.0f;
    return dotProduct(a, b, dim) / denom;
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

    std::vector<float> norms;
    norms.reserve(entries.size());
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
        for (float v : e.embedding) {
            if (std::isnan(v)) {
                err = "FaceSearchLinearIndex: embedding 包含 NaN";
                return false;
            }
        }
        norms.push_back(l2Norm(e.embedding.data(), dim));
    }

    dim_ = dim;
    entries_ = std::move(entries);
    norms_ = std::move(norms);
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
    for (float v : query) {
        if (std::isnan(v)) {
            err = "FaceSearchLinearIndex: query 包含 NaN";
            return {};
        }
    }
    if (topK == 0 || entries_.empty()) return {};
    if (topK > entries_.size()) topK = entries_.size();

    const float qn = opt.assumeL2Normalized ? 1.0f : l2Norm(query.data(), dim_);

    struct FastHit {
        std::size_t index;
        float score;
    };

    std::vector<FastHit> fastHits;
    fastHits.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); i++) {
        const auto& e = entries_[i];
        const float score = cosineSimilarity(query.data(), qn, e.embedding.data(), norms_[i], dim_, opt.assumeL2Normalized);
        fastHits.push_back({i, score});
    }

    const float eps = opt.tieEpsilon >= 0.0f ? opt.tieEpsilon : 0.0f;
    auto comp = [&](const FastHit& a, const FastHit& b) {
        const float da = a.score;
        const float db = b.score;
        const float diff = da - db;
        if (diff > eps) return true;
        if (-diff > eps) return false;
        return a.index < b.index;
    };

    if (fastHits.size() > topK) {
        std::partial_sort(fastHits.begin(), fastHits.begin() + topK, fastHits.end(), comp);
        fastHits.resize(topK);
    } else {
        std::sort(fastHits.begin(), fastHits.end(), comp);
    }

    std::vector<FaceSearchHit> hits;
    hits.reserve(fastHits.size());
    for (const auto& fh : fastHits) {
        hits.push_back(FaceSearchHit{entries_[fh.index].id, fh.index, fh.score});
    }
    return hits;
}

