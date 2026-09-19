#include "FaceSearch.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

// ═══ 生物特征模板就地擦除（安全红线 FACE-ENROLL-009）═══
// 就地清零而**不改变容器大小/容量**：擦除发生在调用方容器析构**之前**，故其析构释放的
// 堆块已是全零，不留模板明文残留；同时保留 size 使调用方/测试可观察到"已擦除"状态。
// id（身份标签）亦属模板内容，一并擦除。
// 实现收敛到 FaceSearch.h 的 `eraseFaceSearchEntrySecrets`——`FaceSearchEntry` 析构
// 亦调用同一函数，两处逻辑同源、不会漂移。
void eraseEntrySecretsInPlace(FaceSearchEntry& entry) {
    eraseFaceSearchEntrySecrets(entry);
}

void eraseEntriesSecretsInPlace(std::vector<FaceSearchEntry>& entries) {
    for (auto& e : entries) eraseEntrySecretsInPlace(e);
}

// 作用域清零守卫（与 native_face_bridge.cpp 同款 RAII 模式）：覆盖所有提前 return 分支
// 与异常展开路径，杜绝"手写漏一个 return"重新引入红线缺陷。
template <typename Fn>
class ScopeClearGuard {
public:
    explicit ScopeClearGuard(Fn fn) : fn_(std::move(fn)) {}
    ~ScopeClearGuard() { fn_(); }
    ScopeClearGuard(const ScopeClearGuard&) = delete;
    ScopeClearGuard& operator=(const ScopeClearGuard&) = delete;

private:
    Fn fn_;
};

template <typename Fn>
ScopeClearGuard<Fn> makeScopeClearGuard(Fn fn) {
    return ScopeClearGuard<Fn>(std::move(fn));
}

}  // namespace

bool FaceSearchLinearIndex::reset(std::vector<FaceSearchEntry>&& entries, std::size_t dim, std::string& err) {
    err.clear();
    // 安全红线（FACE-ENROLL-009）：任何校验失败都必须在容器析构前就地擦除生物特征模板，
    // 否则形参所绑定的调用方容器析构会把 embedding 明文释放到堆上。
    // committed 仅在成功且所有权已移交 entries_ 后置位；此前（即全部失败分支与异常展开
    // 路径）由守卫无条件擦除——守卫覆盖所有 return，杜绝手写漏项。
    bool committed = false;
    auto clearGuard = makeScopeClearGuard([&entries, &committed] {
        if (!committed) eraseEntriesSecretsInPlace(entries);
    });

    if (dim == 0) {
        err = "FaceSearchLinearIndex: dim 不能为 0";
        return false;
    }
    static constexpr std::size_t kMaxLinearEntries = 10000;
    if (entries.size() > kMaxLinearEntries) {
        err = "FaceSearchLinearIndex: N 超过 " + std::to_string(kMaxLinearEntries) + "（当前仅支持线性检索）";
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
    // 成功替换（§2.5-H）：旧 entries_ 的元素在此被析构，其 `~FaceSearchEntry` 先就地清零
    // embedding/id，随后才释放各自堆缓冲 ⇒ 旧库不会以明文被 free。无需在此手写擦除。
    entries_ = std::move(entries);
    norms_ = std::move(norms);
    committed = true;  // 所有权已移交；守卫析构时不再擦除
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

