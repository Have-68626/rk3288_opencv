#include "FaceSearch.h"
#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

static bool nearlyEqual(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

}  // namespace

TEST(FaceSearch, StableTopK) {
    FaceSearchLinearIndex index;

    std::vector<FaceSearchEntry> entries;
    entries.push_back(FaceSearchEntry{"A", {1.0f, 0.0f, 0.0f}});
    entries.push_back(FaceSearchEntry{"B", {1.0f, 0.0f, 0.0f}});
    entries.push_back(FaceSearchEntry{"C", {0.0f, 1.0f, 0.0f}});

    std::string err;
    ASSERT_TRUE(index.reset(std::move(entries), 3, err));

    FaceSearchOptions opt;
    opt.tieEpsilon = 1e-6f;
    opt.assumeL2Normalized = true;

    const std::vector<float> query = {1.0f, 0.0f, 0.0f};
    const auto hits = index.searchTopK(query, 3, opt, err);
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(hits.size(), 3);

    EXPECT_EQ(hits[0].id, "A");
    EXPECT_EQ(hits[1].id, "B");
    EXPECT_EQ(hits[2].id, "C");

    EXPECT_NEAR(hits[0].score, 1.0f, 1e-6f);
    EXPECT_NEAR(hits[1].score, 1.0f, 1e-6f);
    EXPECT_NEAR(hits[2].score, 0.0f, 1e-6f);

    const auto hitsTop2 = index.searchTopK(query, 2, opt, err);
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(hitsTop2.size(), 2);
    EXPECT_EQ(hitsTop2[0].id, "A");
    EXPECT_EQ(hitsTop2[1].id, "B");
}

TEST(FaceSearch, CosineWithoutNormalization) {
    FaceSearchLinearIndex index;

    std::vector<FaceSearchEntry> entries;
    entries.push_back(FaceSearchEntry{"A", {2.0f, 0.0f, 0.0f}});
    entries.push_back(FaceSearchEntry{"B", {1.0f, 1.0f, 0.0f}});
    entries.push_back(FaceSearchEntry{"C", {0.0f, 3.0f, 0.0f}});

    std::string err;
    ASSERT_TRUE(index.reset(std::move(entries), 3, err));

    FaceSearchOptions opt;
    opt.tieEpsilon = 1e-6f;
    opt.assumeL2Normalized = false;

    const std::vector<float> query = {4.0f, 0.0f, 0.0f};
    const auto hits = index.searchTopK(query, 3, opt, err);
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(hits.size(), 3);

    EXPECT_EQ(hits[0].id, "A");
    EXPECT_EQ(hits[1].id, "B");
    EXPECT_EQ(hits[2].id, "C");

    EXPECT_NEAR(hits[0].score, 1.0f, 1e-6f);
    EXPECT_NEAR(hits[1].score, 0.70710677f, 1e-5f);
    EXPECT_NEAR(hits[2].score, 0.0f, 1e-6f);
}

TEST(FaceSearch, NanHandling) {
    FaceSearchLinearIndex index;
    std::string err;

    // Test reset with NaN
    std::vector<FaceSearchEntry> entries_nan;
    entries_nan.push_back(FaceSearchEntry{"A", {1.0f, std::nanf(""), 0.0f}});
    EXPECT_FALSE(index.reset(std::move(entries_nan), 3, err));
    EXPECT_EQ(err, "FaceSearchLinearIndex: embedding 包含 NaN");

    // Reset err for next usage
    err.clear();

    // Test searchTopK with NaN
    std::vector<FaceSearchEntry> entries;
    entries.push_back(FaceSearchEntry{"A", {1.0f, 0.0f, 0.0f}});
    ASSERT_TRUE(index.reset(std::move(entries), 3, err));

    FaceSearchOptions opt;
    opt.tieEpsilon = 1e-6f;
    opt.assumeL2Normalized = true;

    const std::vector<float> query_nan = {1.0f, std::nanf(""), 0.0f};
    const auto hits_nan = index.searchTopK(query_nan, 3, opt, err);
    EXPECT_EQ(err, "FaceSearchLinearIndex: query 包含 NaN");
    EXPECT_TRUE(hits_nan.empty());
}
