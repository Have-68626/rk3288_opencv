#include "FaceSearch.h"
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace {

static bool nearlyEqual(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

// 在原始字节缓冲中查找 ASCII 子串（不触碰任何已释放内存）。
static bool storageContainsBytes(const unsigned char* p, std::size_t n, const std::string& needle) {
    if (needle.empty() || n < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= n; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (p[i + j] != static_cast<unsigned char>(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
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
//   · 因此**若有人删坏桥接层那个 guard，本用例仍会绿**。这是**结构性推论**（本用例与桥接层无任何代码路径相连），
//     不依赖任何带外观测或某轮会话的结论：`native_face_bridge.cpp` 只被 Android `native-lib` 编译
//     （`cmake/android.cmake`），`cmake/core.cmake` 的 `core_unit_tests` 源列表**不含它**，
//     而本文件对 `entriesGuard` / `bridge_` 的引用**仅出现在注释里**。
//   ⇒ 该层暂无法在本套件内补判别用例（亦无 JNI 夹具）。
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

// ═══════════════════════════════════════════════════════════════════════════════
// §2.5-H（模板红线 FACE-ENROLL-009）：**成功替换 / 析构路径**的模板明文残留
// ═══════════════════════════════════════════════════════════════════════════════
//
// 本批擦除发生在**对象内部/析构时**的缓冲上，测试无法读取已释放堆内存。可移植、确定、
// 且不触碰任何已释放内存的判别方式：把 `FaceSearchEntry` placement-new 在一块**自有原始
// 存储**上，手动调用其析构，再扫描该存储中的**对象内联字节**（短 id ⇒ SSO，字符内联于对象，
// 不额外分配）——析构已就地清零则明文消失，否则残留。
//   · 改前红：析构（隐式）不写零，id 明文字节原样留在对象存储里 ⇒ 判别断言失败。
//   · 改后绿：~FaceSearchEntry 先就地清零再销毁成员 ⇒ 明文消失。
// ⚠️ 本用例只覆盖 **对象内联存储**（id）。`embedding` 的堆缓冲在析构内清零后即被
//    `std::vector` 释放，套件内无安全观察手段 ⇒ 该部分由带外分配器探针给出内存级证据
//    （见报告「探针」一节）。
TEST(FaceSearch, EntryDestructorZeroizesInPlaceTemplateBytes) {
    // 全相同字符，保证即使在实现里被写掉首字节（终止符）也仍能定位残留游程。
    const std::string idSecret = "SSSSSSSSSSSS";  // 12 × 'S'
    const std::string needle = "SSSSSS";          // 6 × 'S'
    ASSERT_LT(idSecret.size(), 16u);              // 落在 SSO 区内（MSVC / libstdc++ 均 >= 15）

    alignas(FaceSearchEntry) unsigned char storage[sizeof(FaceSearchEntry)] = {};
    {
        FaceSearchEntry* e = new (storage) FaceSearchEntry{
            idSecret, std::vector<float>{2.0f, 3.0f, 4.0f, 5.0f}};
        // 控制组：析构前明文**必须**可见，否则本用例不具判别力（形同虚设）。
        ASSERT_TRUE(storageContainsBytes(storage, sizeof(storage), needle))
            << "控制组失败：析构前未能在对象存储中定位 id 明文 ⇒ 用例无判别力";
        e->~FaceSearchEntry();
    }
    // 判别组：析构后 id 明文必须已从对象存储中消失（就地清零，未释放对象存储本身）。
    EXPECT_FALSE(storageContainsBytes(storage, sizeof(storage), needle))
        << "析构后对象存储中仍残留 id 明文（FACE-ENROLL-009 析构路径未清零）";
}

// §2.5-H 编译期契约（桥接层需要可拷贝；移动必须保留，否则 vector 增长退化为拷贝）：
static_assert(std::is_copy_constructible<FaceSearchEntry>::value,
              "FaceSearchEntry 必须可拷贝：桥接层 make_shared<const FaceSearchLinearIndex>(s.gallery) 依赖");
static_assert(std::is_move_constructible<FaceSearchEntry>::value,
              "FaceSearchEntry 必须可移动");
static_assert(std::is_copy_assignable<FaceSearchEntry>::value,
              "FaceSearchEntry 必须可复制赋值");
static_assert(std::is_move_assignable<FaceSearchEntry>::value,
              "FaceSearchEntry 必须可移动赋值");
// ⚠️ 「可移动」**不足以保证** vector 增长不退化：std::vector 经 `move_if_noexcept` 选择，
// 只有元素移动**为 noexcept** 时才走移动，否则回退**逐元素深拷贝**（512 维 embedding × 全库）。
// 故必须**单独**钉住 nothrow——这正是 A1/A2 两轮审计各自指出的断言缺口（原断言标题写的
// 「否则退化为拷贝」与 `is_move_constructible` 的语义并不相符）。
static_assert(std::is_nothrow_move_constructible<FaceSearchEntry>::value,
              "FaceSearchEntry 移动构造必须 noexcept：否则 vector 增长经 move_if_noexcept 回退深拷贝");
static_assert(std::is_nothrow_move_assignable<FaceSearchEntry>::value,
              "FaceSearchEntry 移动赋值必须 noexcept");
static_assert(std::is_nothrow_destructible<FaceSearchEntry>::value,
              "FaceSearchEntry 析构必须 noexcept：清零逻辑不得抛，否则 std::terminate");
// C++17 下必须**仍是聚合**——`FaceSearchEntry{id, embedding}` 的聚合初始化依赖它。
// ⚠️ 该性质是 **C++17 专属**：C++20 判据变为「无 user-declared 构造函数」，本类那 5 个
//    `= default` 会让**这条断言在 C++20 下失败**——若将来升标准，它会第一时间报出来（见 FaceSearch.h 注）。
static_assert(std::is_aggregate<FaceSearchEntry>::value,
              "FaceSearchEntry 在 C++17 下必须仍是聚合（聚合初始化 FaceSearchEntry{id,emb} 依赖）");
// ⚠️ **刻意不断言 `!is_trivially_destructible`**（曾加过一条，后经探针证伪为**空断言**，已删）：
//    只要成员仍是 `std::string` / `std::vector<float>`，本类型**恒**为非平凡析构，与该类型自身有没有
//    用户声明的析构**无关** —— 把 `~FaceSearchEntry()` 改成 `= default` 后，那条取反断言**仍然通过**
//    （探针实测 `is_trivially_destructible == 0`），即**永远拦不住任何东西**。
//    「析构被默认化 ⇒ 红线清零静默消失」真正是被**运行期**用例 `EntryDestructorZeroizesInPlaceTemplateBytes`
//    拦住的（RED 探针证其会红），不是编译期。**不要**再把那条空断言加回来。
// 桥接层 native_face_bridge.cpp 以 `FaceSearchEntry entry;` 默认构造（该 TU 不在本 target，无法在此覆盖）。
static_assert(std::is_default_constructible<FaceSearchEntry>::value,
              "FaceSearchEntry 必须可默认构造：桥接层依赖");
static_assert(std::is_copy_constructible<FaceSearchLinearIndex>::value,
              "FaceSearchLinearIndex 必须可拷贝：桥接层不可变快照依赖它");
static_assert(std::is_move_constructible<FaceSearchLinearIndex>::value,
              "FaceSearchLinearIndex 必须可移动");

// §2.5-H：擦除谓词的**套件内**直接单测（确定性强）。它只证明「谓词正确」，不证明「在正确
// 路径上被调用」——后者分别由上方析构用例（类型级、套件内）与带外分配器探针（内存级、
// 覆盖成功替换/索引析构/快照析构）承担。判据：就地清零、size 不变。
TEST(FaceSearch, EraseHelperZeroizesBothEmbeddingAndIdInPlace) {
    FaceSearchEntry e = makeEntry("alice", 8, 1.0f);
    ASSERT_EQ(e.embedding.size(), 8u);
    ASSERT_EQ(e.id.size(), 5u);
    ASSERT_FALSE(isEntryErased(e));  // 前置：确实含明文

    eraseFaceSearchEntrySecrets(e);

    EXPECT_TRUE(isEntryErased(e));        // 已就地清零
    EXPECT_EQ(e.embedding.size(), 8u);    // 就地：不改变大小
    EXPECT_EQ(e.id.size(), 5u);
}

// §2.5-H 代价链守卫：`~FaceSearchEntry` 不得抑制移动。判据：移动后目标**复用源缓冲**
// （data() 指针相同）⇒ 确为缓冲偷取而非深拷贝（若退化为拷贝则指针必然不同）。
TEST(FaceSearch, MoveStealsEmbeddingBufferNotDegradedToCopy) {
    FaceSearchEntry a = makeEntry("alice", 512, 1.0f);
    const float* before = a.embedding.data();
    ASSERT_NE(before, nullptr);

    FaceSearchEntry b = std::move(a);

    EXPECT_EQ(b.embedding.data(), before) << "移动被抑制，退化为深拷贝（vector 增长将 O(n) 拷贝）";
    EXPECT_EQ(a.embedding.size(), 0u);  // 源已交出缓冲（不留明文副本）
    EXPECT_EQ(b.embedding.size(), 512u);
}

// §2.5-H：复刻桥接层 `make_shared<const FaceSearchLinearIndex>(s.gallery)` 的快照拷贝路径。
// 作用：①编译期证明「索引 + FaceSearchEntry 可拷贝」（若拷贝被删则本用例不编译）；
//      ②运行期证明快照与原索引独立（快照析构不破坏原索引）。
// ⚠️ 快照析构时**堆缓冲的明文清零**无法在套件内安全观察 ⇒ 由带外探针给出内存级证据。
TEST(FaceSearch, IndexSnapshotCopySupportedAndIndependent) {
    FaceSearchLinearIndex idx;
    std::vector<FaceSearchEntry> entries;
    entries.push_back(makeEntry("alice", 8, 1.0f));
    std::string err;
    ASSERT_TRUE(idx.reset(std::move(entries), 8, err));

    auto snap = std::make_shared<const FaceSearchLinearIndex>(idx);  // 桥接层同款快照拷贝
    ASSERT_EQ(snap->size(), 1u);
    ASSERT_EQ(snap->dim(), 8u);

    snap.reset();  // 快照析构：其内嵌模板缓冲经 ~FaceSearchEntry 清零后释放

    EXPECT_EQ(idx.size(), 1u);  // 原索引不受影响
    ASSERT_EQ(idx.entries().size(), 1u);
    EXPECT_EQ(idx.entries()[0].id, "alice");
}

