#include "FaceSearch.h"
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace {

static bool nearlyEqual(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

TEST(FaceSearchFiniteInputs, RejectsInfiniteTemplatesWithoutReplacingGallery) {
    for (float invalid : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
        FaceSearchLinearIndex index;
        std::string err;
        ASSERT_TRUE(index.reset({FaceSearchEntry{"valid", {1.0f, 0.0f}}}, 2, err));
        std::vector<FaceSearchEntry> entries{{"invalid", {invalid, 1.0f}}};
        ASSERT_FALSE(index.reset(std::move(entries), 2, err));
        EXPECT_FALSE(err.empty());
        ASSERT_EQ(index.size(), 1u);
        EXPECT_EQ(index.entries()[0].id, "valid");
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_FLOAT_EQ(entries[0].embedding[0], 0.0f);
        EXPECT_FLOAT_EQ(entries[0].embedding[1], 0.0f);
    }
}

TEST(FaceSearchFiniteInputs, RejectsInfiniteQueriesForEitherNormalizationMode) {
    FaceSearchLinearIndex index;
    std::string err;
    ASSERT_TRUE(index.reset({FaceSearchEntry{"valid", {1.0f, 0.0f}}}, 2, err));
    for (bool normalized : {false, true}) {
        FaceSearchOptions options;
        options.assumeL2Normalized = normalized;
        for (float invalid : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
            const auto hits = index.searchTopK({invalid, 1.0f}, 1, options, err);
            EXPECT_TRUE(hits.empty());
            EXPECT_FALSE(err.empty());
        }
    }
    EXPECT_EQ(index.searchTopK({1.0f, 0.0f}, 1, FaceSearchOptions{}, err).size(), 1u);
    EXPECT_TRUE(err.empty());
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
//    （一次性带外验证手段，不随仓内测试保留）。
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

// ═══════════════════════════════════════════════════════════════════════════════
// FACE-ENROLL-009（任务 B 补缺）：**成功替换路径**旧模板堆缓冲的清零——套件内直测
// ═══════════════════════════════════════════════════════════════════════════════
//
// 缺口（本文件上方已自陈）：reset() 成功替换（`FaceSearchLinearIndex::reset` 对 `entries_` 的整体替换）
// 时，旧 entries_ 的元素被析构 —— `~FaceSearchEntry` 先**就地清零** embedding/id、再释放各自堆缓冲。该
// 「旧 embedding 堆缓冲已清零」此前**只在套件外**由一次性分配器探针给出内存级证据；套件内既有析构用例
// （`EntryDestructorZeroizesInPlaceTemplateBytes`）只钉「析构就地清零」这一**类型级**性质，**并不**覆盖
// 「**成功替换路径**会析构旧元素」这一调用路径 ⇒ 本用例补的正是后者（套件内此前**无成功替换路径**的观察手段）。
//
// 本用例把该观测**收回套件内**：通过一个进程级 `operator new/delete` 拦截器，在**被测嵌入缓冲
// 即将被 free 的那一刻**把它的字节 memcpy 到测试自有缓冲（快照后照常 free ⇒ **绝不读取已释放内存**，
// 与上方析构用例「不触碰已释放内存」的口径一致），随后只对**快照**做断言。
//   · 控制组：一条**不经过任何清零路径**的对照缓冲，其在 free 时应被快照到明文（非全零）⇒ 证明
//     拦截器确实能观察到「未清零」的缓冲（否则本用例的判据形同虚设）。
//   · 判别组：被测旧模板缓冲在 free 时的快照必须**全零**。
//
// 判别力：短路该清零（把 `~FaceSearchEntry` 里的 `eraseFaceSearchEntrySecrets(*this)` 注释掉，
// 或把成功路径改成不析构旧元素）⇒ 快照保留明文 ⇒ 本用例翻红；还原 ⇒ 绿。
//
// ⚠️ 代价与边界（如实登记）：
//   ① 该 `operator new/delete` 是**进程级替换**，会对整个 core_unit_tests 二进制生效；实现为
//      malloc/free 透传 + 一个「单指针观察窗」，未命中时零额外行为（新增/删除行为与默认一致）。
//      over-aligned 分配走默认的对齐版 new/delete（未替换），与本次标量替换互不交叉。
//   ② 观察窗恒定在测试作用域内 arm/disarm，gtest 默认顺序执行 ⇒ 无并发交错。
//   ③ 它证明「旧模板缓冲在成功替换语义下被清零」，判据落在**释放那一刻的字节**上；不依赖分配器
//      是否复用该块、也不读已释放内存。
namespace {
void* g_faceSearchWatchPtr = nullptr;      // 待观测的旧 embedding 缓冲起始地址
std::size_t g_faceSearchWatchBytes = 0;    // 其字节数（须 ≤ 快照容量）
bool g_faceSearchArmed = false;            // 观察窗开关（未命中时拦截器零副作用）
bool g_faceSearchCaptured = false;         // 是否已捕获到该缓冲的释放
unsigned char g_faceSearchSnapshot[128]{}; // 快照（embedding 维很小，缓冲远小于此）

bool allZeroBytes(const unsigned char* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != 0u) return false;
    }
    return true;
}
}  // namespace

// 进程级替换：malloc/free 透传；命中观察窗时先把字节快照下来再照常释放。
void* operator new(std::size_t n) {
    void* p = std::malloc(n ? n : 1u);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept {
    if (p != nullptr && g_faceSearchArmed && p == g_faceSearchWatchPtr) {
        std::memcpy(g_faceSearchSnapshot, p, g_faceSearchWatchBytes);
        g_faceSearchCaptured = true;
    }
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

TEST(FaceSearch, SuccessfulReplaceZeroizesOldEmbeddingHeapBuffer) {
    FaceSearchLinearIndex idx;
    std::string err;

    // 装入一条旧模板（embedding 恒非零）。
    std::vector<FaceSearchEntry> oldEntries;
    oldEntries.push_back(makeEntry("alice", 8, 1.0f));
    ASSERT_TRUE(idx.reset(std::move(oldEntries), 8, err)) << err;
    ASSERT_EQ(idx.size(), 1u);

    // 取旧 embedding 堆缓冲（其所属元素即将被下一次成功替换析构）。
    const float* oldBuf = idx.entries()[0].embedding.data();
    ASSERT_NE(oldBuf, nullptr);
    const std::size_t oldBytes = idx.entries()[0].embedding.size() * sizeof(float);
    ASSERT_GT(oldBytes, 0u);
    ASSERT_LE(oldBytes, sizeof(g_faceSearchSnapshot));

    // 控制组：拦截器对「未清零」缓冲确有判别力——一条不经过任何清零路径的对照缓冲，
    // 在释放时快照应保留明文（非全零），否则本用例的判据不可信。
    {
        auto* plain = new std::vector<float>(8, 7.0f);
        const float* plainBuf = plain->data();
        g_faceSearchWatchPtr = const_cast<void*>(static_cast<const void*>(plainBuf));
        g_faceSearchWatchBytes = plain->size() * sizeof(float);
        g_faceSearchArmed = true;
        g_faceSearchCaptured = false;
        delete plain;  // 释放对照缓冲（无清零路径）
        g_faceSearchArmed = false;
        ASSERT_TRUE(g_faceSearchCaptured) << "控制组：未捕获到对照缓冲的释放";
        EXPECT_FALSE(allZeroBytes(g_faceSearchSnapshot, g_faceSearchWatchBytes))
                << "控制组失败：未清零的对照缓冲快照却是全零 ⇒ 拦截器不可信，本用例无判别力";
    }

    // 判别组：成功替换（install 一条新模板）。观察窗覆盖 reset 调用**及其后调用方容器的析构**——
    // 以防实现把旧缓冲 swap 回调用方容器、留到作用域末才释放（两臂都会经 ~FaceSearchEntry 清零）。
    g_faceSearchWatchPtr = const_cast<void*>(static_cast<const void*>(oldBuf));
    g_faceSearchWatchBytes = oldBytes;
    g_faceSearchArmed = true;
    g_faceSearchCaptured = false;
    {
        std::vector<FaceSearchEntry> newEntries;
        newEntries.push_back(makeEntry("bob", 8, 5.0f));
        ASSERT_TRUE(idx.reset(std::move(newEntries), 8, err)) << err;
        ASSERT_EQ(idx.size(), 1u);          // 成功替换：新模板就位
        EXPECT_EQ(idx.entries()[0].id, "bob");
    }  // newEntries 在此析构（若上面那次替换以 swap 承载旧缓冲，旧缓冲会于此释放）
    g_faceSearchArmed = false;

    ASSERT_TRUE(g_faceSearchCaptured)
            << "未捕获到旧模板缓冲的释放：成功替换未析构旧 entries_（或旧缓冲未按预期释放）";
    EXPECT_TRUE(allZeroBytes(g_faceSearchSnapshot, oldBytes))
            << "成功替换后旧模板堆缓冲未被清零（FACE-ENROLL-009 析构路径未生效）";
}

