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

