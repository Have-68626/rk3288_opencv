#include "FaceSearch.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

static bool nearlyEqual(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

}  // namespace

bool test_face_search_stable_topk() {
    FaceSearchLinearIndex index;

    std::vector<FaceSearchEntry> entries;
    entries.push_back(FaceSearchEntry{"A", {1.0f, 0.0f, 0.0f}});
    entries.push_back(FaceSearchEntry{"B", {1.0f, 0.0f, 0.0f}});
    entries.push_back(FaceSearchEntry{"C", {0.0f, 1.0f, 0.0f}});

    std::string err;
    if (!index.reset(std::move(entries), 3, err)) return false;

    FaceSearchOptions opt;
    opt.tieEpsilon = 1e-6f;
    opt.assumeL2Normalized = true;

    const std::vector<float> query = {1.0f, 0.0f, 0.0f};
    const auto hits = index.searchTopK(query, 3, opt, err);
    if (!err.empty()) return false;
    if (hits.size() != 3) return false;

    if (hits[0].id != "A") return false;
    if (hits[1].id != "B") return false;
    if (hits[2].id != "C") return false;

    if (!nearlyEqual(hits[0].score, 1.0f)) return false;
    if (!nearlyEqual(hits[1].score, 1.0f)) return false;
    if (!nearlyEqual(hits[2].score, 0.0f)) return false;

    const auto hitsTop2 = index.searchTopK(query, 2, opt, err);
    if (!err.empty()) return false;
    if (hitsTop2.size() != 2) return false;
    if (hitsTop2[0].id != "A") return false;
    if (hitsTop2[1].id != "B") return false;

    return true;
}

bool test_face_search_cosine_without_normalization() {
    FaceSearchLinearIndex index;

    std::vector<FaceSearchEntry> entries;
    entries.push_back(FaceSearchEntry{"A", {2.0f, 0.0f, 0.0f}});
    entries.push_back(FaceSearchEntry{"B", {1.0f, 1.0f, 0.0f}});
    entries.push_back(FaceSearchEntry{"C", {0.0f, 3.0f, 0.0f}});

    std::string err;
    if (!index.reset(std::move(entries), 3, err)) return false;

    FaceSearchOptions opt;
    opt.tieEpsilon = 1e-6f;
    opt.assumeL2Normalized = false;

    const std::vector<float> query = {4.0f, 0.0f, 0.0f};
    const auto hits = index.searchTopK(query, 3, opt, err);
    if (!err.empty()) return false;
    if (hits.size() != 3) return false;

    if (hits[0].id != "A") return false;
    if (hits[1].id != "B") return false;
    if (hits[2].id != "C") return false;

    if (!nearlyEqual(hits[0].score, 1.0f)) return false;
    if (!nearlyEqual(hits[1].score, 0.70710677f, 1e-5f)) return false;
    if (!nearlyEqual(hits[2].score, 0.0f)) return false;

    return true;
}

bool test_face_search_nan_handling() {
    FaceSearchLinearIndex index;
    std::string err;

    // Test reset with NaN
    std::vector<FaceSearchEntry> entries_nan;
    entries_nan.push_back(FaceSearchEntry{"A", {1.0f, std::nanf(""), 0.0f}});
    if (index.reset(std::move(entries_nan), 3, err)) return false;
    if (err != "FaceSearchLinearIndex: embedding 包含 NaN") return false;

    // Test searchTopK with NaN
    std::vector<FaceSearchEntry> entries;
    entries.push_back(FaceSearchEntry{"A", {1.0f, 0.0f, 0.0f}});
    if (!index.reset(std::move(entries), 3, err)) return false;

    FaceSearchOptions opt;
    opt.tieEpsilon = 1e-6f;
    opt.assumeL2Normalized = true;

    const std::vector<float> query_nan = {1.0f, std::nanf(""), 0.0f};
    const auto hits_nan = index.searchTopK(query_nan, 3, opt, err);
    if (err != "FaceSearchLinearIndex: query 包含 NaN") return false;
    if (!hits_nan.empty()) return false;

    return true;
}
