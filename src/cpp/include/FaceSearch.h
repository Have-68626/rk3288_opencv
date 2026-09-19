#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct FaceSearchEntry {
    std::string id;
    std::vector<float> embedding;
};

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

