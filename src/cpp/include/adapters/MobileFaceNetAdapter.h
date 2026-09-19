#pragma once

#include "Embedder.h"

#include <mutex>
#include <string>

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
#include <net.h>
#endif

namespace rk_core {

class MobileFaceNetAdapter : public Embedder {
public:
    MobileFaceNetAdapter();
    ~MobileFaceNetAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    std::optional<std::vector<float>> embed(const cv::Mat& alignedFaceBgr, std::string& err) override;
    int embeddingDim() const override;  // 实现在 .cpp：需持锁读 expectedDim_（A2 审计 M2）
    const char* name() const override;

    // 期望维度为 0 或负数时表示「不校验相等性」；此时 `embeddingDim()` 回落到默认维度而不返回 0
    // （返回 0 会让按它建索引/判尺寸的调用方拿到一个不可能的维度 —— 子代理 4 S5）。
    static constexpr int kDefaultEmbeddingDim = 512;

    // ── §2.5-A：节点名与输出维度必须可注入，且默认值要与本仓模型一致 ──
    // 改前本适配器把 `ex.input("data")` / `ex.extract("output")` 与「输出必须恰 128 维」
    // **硬编码**在实现里，而本仓两个 mobilefacenet 模型实为：
    //   · 节点名 `in0` / `out0`（.param 首层 `Input in0`、末层 `InnerProduct … out0`）；
    //   · 输出 **512** 维（两个 .param 的末层均为 `0=512`）。
    // ⇒ 旧实现对该模型**必然**在 `ex.input` 即失败，且没有任何调用方能改。
    // 语义与 `ArcFaceAdapter::setNodeNames` 一致：**空串忽略**（保留既有值，不清空）。
    // ⚠️ 配置时机：节点名只在 `embed()` 里被读取（`load()` 根本不使用它们），故**在首次 embed 前**
    //    配置即可；此处写「推荐在 load() 前」是沿用调用方习惯，**不是**代码强制的约束。
    // ⚠️ **诊断可见性（如实记录，勿高估）**：`embed()` 的各类 err 文本**是否有人看得到，取决于调用方**。
    //    本仓生产路径上：`Engine.cpp` 的两处 embed 失败分支只清零再 `continue`（**不记日志、不外传**）；
    //    `FaceInferStages.cpp` 的 INT8 回退只记一行通用 warn；`RealtimeFacePipeline` 侧经
    //    `normalizeReasonCode` 会把任何非固定码**归一为 `runtime_init_failed`**。
    //    ⇒ 差异化诊断**到不了日志或 ACCEL_SELF_CHECK**（子代理 4 S2，已记为独立待办，本批不扩大范围）。
    void setNodeNames(const std::string& input, const std::string& output);

    // 当前生效的输入/输出节点名（诊断与测试用）。
    // ⚠️ **按值返回**，与 `ArcFaceAdapter` 的 `const std::string&` 刻意不同：本类的 setter 持锁写，
    //    若 getter 返回内部引用，调用方持有的引用可能在并发 `setNodeNames` 时**悬垂**——
    //    「写加锁、读不加锁且退引用」是半吊子的线程安全，比全不加锁更危险（A2 审计 M2）。
    //    故本类三个 getter 一律**持锁 + 拷贝值**返回。
    std::string inputNodeName() const;
    std::string outputNodeName() const;

    // 期望的嵌入维度。默认 512（与本仓模型一致）；`embed()` 只在**不符**时失败并报出**实际**维度，
    // 不再用硬编码常量把「模型换维度」这类正常演进误判为不可用。
    // ⚠️ `<= 0` **只关闭「相等性」校验，不关闭尺寸上限**：`embed()` 仍会在分配**之前**拒绝超过
    //    内部上限 `kMaxReasonableDim` 的输出维度 —— 否则「关掉校验」就等于把分配大小完全交给
    //    模型输出（损坏/恶意 .param 可致巨额分配），而旧代码那个硬编码的 `128` **顺带**正是一条
    //    分配上界（A2 审计 M3）。
    void setExpectedEmbeddingDim(int dim);
    int expectedEmbeddingDim() const;

private:
    mutable std::mutex mu_;
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    ncnn::Net net_;
#endif
    bool loaded_ = false;
    std::string currentName_;
    int inputW_ = 112;
    int inputH_ = 112;
    std::string inputName_ = "in0";
    std::string outputName_ = "out0";
    int expectedDim_ = kDefaultEmbeddingDim;
};

} // namespace rk_core
