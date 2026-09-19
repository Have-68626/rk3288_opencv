#include "FaceSearch.h"
#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

static bool nearlyEqual(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

// ── FACE-ENROLL-009 辅助：构造可识别为非零的模板，并判定其是否已被就地擦除 ──
static FaceSearchEntry makeEntry(std::string id, std::size_t dim, float base) {
    std::vector<float> embedding(dim);
    for (std::size_t i = 0; i < dim; ++i) {
        embedding[i] = base + static_cast<float>(i) + 1.0f;  // 恒为非零
    }
    return FaceSearchEntry{std::move(id), std::move(embedding)};
}

// 就地擦除判据：容器 size 保留，但 embedding 全为 0、id 全为 '\0'。
// （就地进行故无需读取已释放内存，判据完全可移植、确定。）
static bool isEntryErased(const FaceSearchEntry& e) {
    for (float v : e.embedding) {
        if (v != 0.0f) return false;
    }
    for (char c : e.id) {
        if (c != '\0') return false;
    }
    return true;
}

static bool areAllEntriesErased(const std::vector<FaceSearchEntry>& entries) {
    for (const auto& e : entries) {
        if (!isEntryErased(e)) return false;
    }
    return true;
}

// reset 的全部失败分支（须与 FaceSearch.cpp 中 reset 的 early-return 一一对应）。
struct ResetFailCase {
    const char* name;
    std::size_t dim;
    std::vector<FaceSearchEntry> entries;
    const char* errSubstr;
};

static std::vector<ResetFailCase> makeResetFailCases() {
    std::vector<ResetFailCase> cases;

    {  // 分支 1：dim == 0
        std::vector<FaceSearchEntry> es;
        es.push_back(makeEntry("alice", 3, 1.0f));
        cases.push_back(ResetFailCase{"dim_zero", 0, std::move(es), "dim 不能为 0"});
    }
    {  // 分支 2：条目数超过线性索引上限 10000
        std::vector<FaceSearchEntry> es;
        for (int i = 0; i < 10001; ++i) {
            es.push_back(makeEntry("p" + std::to_string(i), 3, 1.0f));
        }
        cases.push_back(ResetFailCase{"too_many_entries", 3, std::move(es), "N 超过"});
    }
    {  // 分支 3：存在空 id
        std::vector<FaceSearchEntry> es;
        es.push_back(makeEntry("alice", 3, 1.0f));
        es.push_back(makeEntry("", 3, 2.0f));
        cases.push_back(ResetFailCase{"empty_id", 3, std::move(es), "空 id"});
    }
    {  // 分支 4：embedding 维度不一致
        std::vector<FaceSearchEntry> es;
        es.push_back(makeEntry("alice", 3, 1.0f));
        es.push_back(makeEntry("bob", 4, 2.0f));
        cases.push_back(ResetFailCase{"dim_mismatch", 3, std::move(es), "embedding dim 不一致"});
    }
    {  // 分支 5：embedding 含 NaN
        std::vector<FaceSearchEntry> es;
        es.push_back(makeEntry("alice", 3, 1.0f));
        FaceSearchEntry nanEntry = makeEntry("bob", 3, 2.0f);
        nanEntry.embedding[1] = std::nanf("");
        es.push_back(std::move(nanEntry));
        cases.push_back(ResetFailCase{"nan_embedding", 3, std::move(es), "包含 NaN"});
    }

    return cases;
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

// ── FACE-ENROLL-009：失败路径必须就地擦除生物特征模板（embedding/id）──
//
// 可测性：擦除发生在容器析构**之前**（就地清零、保留 size），故测试无需读取已释放内存、
// 无需自定义分配器、也无需测试接缝——直接检查调用方容器即可获得确定性判据。
// 改前（按值传参 + 无擦除）时形参已被 move 走、调用方容器被搬空 ⇒ 下方 size 断言即红；
// 若仅去掉擦除而保留右值引用形参 ⇒ 内容断言即红。
TEST(FaceSearch, ResetFailureErasesBiometricTemplate) {
    FaceSearchLinearIndex index;
    std::string err;

    for (auto& c : makeResetFailCases()) {
        const std::string tag = c.name;
        const std::size_t expectedSize = c.entries.size();
        ASSERT_GT(expectedSize, 0u) << tag;

        ASSERT_FALSE(index.reset(std::move(c.entries), c.dim, err)) << tag;
        EXPECT_FALSE(err.empty()) << tag;
        EXPECT_NE(err.find(c.errSubstr), std::string::npos) << tag << " err=" << err;

        // 契约：失败路径不得搬空/消费调用方容器（否则其缓冲无从擦除，也无从观察）。
        EXPECT_EQ(c.entries.size(), expectedSize)
            << tag << "：失败路径不得搬空调用方容器";
        // 红线：所有失败分支都必须就地擦除 embedding（及 id）。
        EXPECT_TRUE(areAllEntriesErased(c.entries))
            << tag << "：失败后仍残留生物特征模板明文";
    }
}

// 判别用例（**等价复刻**桥接层 `bridge_replaceGallery` 的调用形态，**非端到端**）：桥接层把局部 entries
// 以 std::move 传入 reset；失败时由 reset 自行就地擦除，而非依赖桥接层那个 `entriesGuard`。
// 去掉 reset 的守卫 ⇒ 本用例红。
//
// ⚠️ **覆盖缺口（如实标注而非掩盖）**：本用例**未**调用 `bridge_replaceGallery`，故
//   · **不覆盖**桥接层 `entriesGuard`——它只在**循环内 early-return** 时才是真覆盖，那种场景本用例到不了；
//   · 因此**若有人删坏桥接层那个 guard，本用例仍会绿**（子代理3 探针已实证）。
//   桥接层仅由 Android `native-lib` 目标编译（无 Windows target），`core_unit_tests` 不含它、
//   也无 JNI 夹具 ⇒ 该层暂无法在本套件内补判别用例。
TEST(FaceSearch, BridgeResetFailurePathErasesTemplate) {
    FaceSearchLinearIndex gallery;
    std::vector<FaceSearchEntry> entries;
    entries.push_back(makeEntry("alice", 512, 1.0f));
    entries.push_back(makeEntry("bob", 512, 2.0f));

    std::string err;
    ASSERT_FALSE(gallery.reset(std::move(entries), 0, err));  // dim=0 触发失败

    ASSERT_EQ(entries.size(), 2u);  // 失败不搬空（仍可观察其缓冲）
    EXPECT_TRUE(areAllEntriesErased(entries));
}
