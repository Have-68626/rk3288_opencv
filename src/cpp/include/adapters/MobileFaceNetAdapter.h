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
    //    本仓消费点的统一口径是：失败即丢弃该帧样本（模板红线要求丢弃前把向量清零），err 文本
    //    **只记本地日志、不向调用方外传**（不回传 Java、不进 ACCEL_SELF_CHECK）。据此：
    //      · `Engine.cpp` 的两处 embed 失败分支（`captureEnrollmentFaces` / `assignRealtimeIdentities`）
    //        均为「清零 + 限频记日志」，经 `Engine::noteEmbedError(site, err)` 按时间窗限频；
    //      · `FaceInferStages.cpp` 的 INT8 回退把 load / embed 两段 err 并进一行 warn；
    //      · `RealtimeFacePipeline` 侧经 `normalizeReasonCode` 会把任何非固定码**归一为
    //        `runtime_init_failed`**。
    //    ⇒ 差异化诊断**不会**出现在 Java 侧错误消息或 ACCEL_SELF_CHECK 里；且**任一消费点只
    //      `continue` 而未记日志时，该路径上这些文本就完全不可见**——改动消费点时请逐点确认。
    //      （同理：逐帧 × 每脸路径若丢掉 `noteEmbedError` 的限频，会退化成一帧多行日志。）
    //    ⇒ 改动任何消费点时请保持「清零 + 限频记日志、不外传」，否则本段差异化文本会重新退化为
    //      无诊断价值（子代理 4 S2）。
    void setNodeNames(const std::string& input, const std::string& output);

    // 当前生效的输入/输出节点名（诊断与测试用）。
    // ⚠️ **按值返回**，与 `ArcFaceAdapter` 的 `const std::string&` 刻意不同：本类的 setter 持锁写，
    //    若 getter 返回内部引用，调用方持有的引用可能在并发 `setNodeNames` 时**悬垂**——
    //    「写加锁、读不加锁且退引用」是半吊子的线程安全，比全不加锁更危险（A2 审计 M2）。
    //    故本类所有取值型 getter（`inputNodeName` / `outputNodeName` / `expectedEmbeddingDim` /
    //    `embeddingDim`）一律**持锁 + 拷贝值**返回。
    // ⚠️ `name()` 是**例外**，勿按上面这句推断：受基类 `Embedder::name()` 的 `const char*` 接口约束，
    //    它持锁读取但返回的是 `currentName_` 的**内部缓冲指针**（不拷贝）——该指针在下一次
    //    `load()` 改写 `currentName_` 或对象析构后即失效，调用方不得跨 `load()` 持有。
    std::string inputNodeName() const;
    std::string outputNodeName() const;

    // 期望的嵌入维度。默认 512（与本仓模型一致）；`embed()` 只在**不符**时失败并报出**实际**维度，
    // 不再用硬编码常量把「模型换维度」这类正常演进误判为不可用。
    // ⚠️ `<= 0` **只关闭「相等性」校验，不关闭尺寸上限**：`embed()` 仍会拒绝超过内部上限
    //    `kMaxReasonableDim` 的输出维度（该闸门在相等性判断之前、无条件执行）。
    //    ⚠️ **别把它读成「分配之前」的防线**：ncnn 的 `ex.extract` 已先按模型输出形状完成了
    //    第一次分配，闸门真正界定的是其后的**复制缓冲** `std::vector<float>` 与 `memcpy` 长度
    //    （详见 MobileFaceNetAdapter.cpp 中 `embed()` 内的长注释；子代理 4 S4 修正了此处原先
    //    「在分配之前拒绝 ⇒ 可防损坏/恶意 .param 巨额分配」的说法）。旧代码那个硬编码的 `128`
    //    **顺带**起过同样的「第二份分配上界」作用（A2 审计 M3）。
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
