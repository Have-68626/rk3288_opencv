#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

struct FaceSearchEntry;

// 就地擦除一条模板（embedding 全 0、id 全 '\0'），**不改变容器大小/容量、不释放任何缓冲**。
// FACE-ENROLL-009 的**单点实现**：`FaceSearchEntry` 析构（覆盖**本类型**的全部释放路径）与
// FaceSearch.cpp 中 reset 失败路径的守卫共用同一段逻辑，避免两处实现漂移。
// ⚠️ 「本类型」是必要限定：`id` 的**副本**（`FaceSearchHit::id`、桥接 `keepIds`）不在此列，见下方结构体注。
inline void eraseFaceSearchEntrySecrets(FaceSearchEntry& entry);

struct FaceSearchEntry {
    std::string id;
    std::vector<float> embedding;

    // ── 安全红线 FACE-ENROLL-009（§2.5-H）：析构时**就地清零**模板 ──
    // 把清零挂在**类型**的析构上，能一次性覆盖**本类型**的全部释放路径：
    //   ① reset 成功替换（旧 entries_ 被 move 赋值析构、缓冲 deallocate）；
    //   ② FaceSearchLinearIndex 析构（**改前**整库以明文被 free；现由本析构先行清零）；
    //   ③ 桥接层 make_shared<const FaceSearchLinearIndex>(s.gallery) 的快照析构；
    //   ④ reset 失败路径（此前已修；此处成为冗余但无害的第二道防线）；
    //   ⑤ 本头文件之外的一切 FaceSearchEntry 释放点（FaceInferStages / main 等）。
    // 逐点手写擦除正是本缺陷的根因（上一批只修了 ④，漏掉 ①②③）——类型级析构对本类型**不会「再漏一处」**。
    // 析构体先清零，随后成员析构才释放 embedding/id 的堆缓冲，故被释放的**堆块**不含模板明文。
    // ⚠️ **边界（勿读成「所有 id 明文均已覆盖」）——三条，逐条如实**：
    //   ① `id` 的**副本**不在其列：`searchTopK` 返回的 `FaceSearchHit::id` 与桥接层 `keepIds`
    //      都是普通 `std::string`，其容器析构**不清零**（按「不扩大范围」的决定只记录，待办见 §2.5）。
    //      embedding（红线首要对象）不在这些副本路径上。
    //   ② **moved-from 对象的 SSO 内联 id 会残留**（子代理3 用探针实证）：`FaceSearchEntry` 在
    //      `vector` 增长 / `push_back(std::move(..))` / 桥接 `entries.push_back(std::move(entry))`
    //      时**例行被 move**；被移出对象的**短 id** 满足 `size() == 0`，而其 SSO 缓冲仍留着旧字节，
    //      于是 `fill(id.begin(), id.end(), …)` **是 no-op** ⇒ 该对象析构时内联区仍有 id 明文尾。
    //      （长 id 走堆、移动时缓冲被偷，故源侧无残留；embedding 移动后 `capacity == 0`，亦无。）
    //      **判 Low**（仅身份标签、非 embedding），且标准不提供改写 `[size, capacity)` 的手段
    //      ⇒ **只如实记录、不修**。
    //   ③ `capacity > size` 的 slack 区（未构造元素）不在清零范围内：embedding 经
    //      `vector<float>(dim)` / `resize(dim)` 构造（实践上 capacity == size，但**标准并不保证**），
    //      当前无已知可达的明文残留路径。
    // 注：`FaceSearchLinearIndex::norms_`（L2 范数）亦不清零：它是**派生标量**（1 float/身份），
    //     不可逆推 512 维模板，不属红线对象。
    ~FaceSearchEntry() { eraseFaceSearchEntrySecrets(*this); }

    // 显式声明全部五个特殊成员（缺一不可；"四个"通常指拷贝/移动构造 + 拷贝/移动赋值，
    // 此处连**默认构造**一并显式化，故共 5 条声明）：
    //   · 用户声明析构 ⇒ 抑制隐式**移动**，只加析构会让 std::vector<FaceSearchEntry> 在
    //     增长/重分配与 push_back(std::move(...)) 时退化为逐元素**拷贝**；
    //   · 显式声明移动 ⇒ 隐式**拷贝被删除**，而桥接层 make_shared<const
    //     FaceSearchLinearIndex>(s.gallery) 需要整个索引可拷贝 ⇒ 必须同时声明拷贝。
    // 五个一并 `= default`，行为与「原隐式版本」逐位一致。类型**在 C++17 下仍为聚合**：
    // `= default` 属 user-declared 而非 user-provided，而 C++17 聚合判据只排除
    // **user-provided / explicit / inherited** 构造函数（**析构函数不在其中**），
    // 故 `FaceSearchEntry{id, embedding}` 的聚合初始化继续有效（已由 static_assert(is_aggregate) 钉住）。
    // ⚠️ **C++20 起判据变为「无 user-declared 构造函数」** ⇒ 本类这 5 个显式 `= default` 会让它
    //    **不再是聚合**，届时所有 `FaceSearchEntry{a, b}` 聚合初始化会编译失败（编译期暴露、非静默）。
    //    若将来升到 C++20，须删掉这几个声明或改造调用点。
    FaceSearchEntry() = default;
    FaceSearchEntry(const FaceSearchEntry&) = default;
    FaceSearchEntry(FaceSearchEntry&&) = default;
    FaceSearchEntry& operator=(const FaceSearchEntry&) = default;
    FaceSearchEntry& operator=(FaceSearchEntry&&) = default;
};

inline void eraseFaceSearchEntrySecrets(FaceSearchEntry& entry) {
    std::fill(entry.embedding.begin(), entry.embedding.end(), 0.0f);
    std::fill(entry.id.begin(), entry.id.end(), '\0');
}

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

    // 右值引用形参（按引用）：调用方以 std::move / 临时量显式移交所有权。
    // 失败路径**就地擦除** embedding/id（安全红线 FACE-ENROLL-009：生物特征模板不得残留
    // 在随后被释放的堆内存中），且**不搬空**形参——调用方在失败后仍可观察到"已擦除"的容器。
    // 成功路径把缓冲移入 entries_。不用非 const 左值引用 `std::vector<FaceSearchEntry>&`：
    // 它既**绑不上**大括号临时量 `reset({...}, dim, err)`，也**绑不上** `reset(std::move(x), ...)`
    // ——两类调用点都会直接编译失败（审计实测：13 处大括号临时量 + 14 处 std::move）。
    // 也**不用按值**：按值虽同样能接受这两种形态、且 rvalue 实参走**移动**（**不产生元素拷贝**，
    // 分配器探针实测 0 次），但它会在**失败时把调用方容器搬空** ⇒ 调用方与测试都**无法观察
    // 「已擦除」状态**——而本红线修复的可验证性正建立在「该状态可见」之上。
    // （`const&` 则会强制拷贝出一份模板明文副本，且无法搬移，同样排除。）
    bool reset(std::vector<FaceSearchEntry>&& entries, std::size_t dim, std::string& err);

    std::size_t dim() const { return dim_; }
    std::size_t size() const { return entries_.size(); }
    const std::vector<FaceSearchEntry>& entries() const { return entries_; }

    std::vector<FaceSearchHit> searchTopK(const std::vector<float>& query, std::size_t topK, const FaceSearchOptions& opt, std::string& err) const;

private:
    std::size_t dim_ = 0;
    std::vector<FaceSearchEntry> entries_;
    std::vector<float> norms_;
};

