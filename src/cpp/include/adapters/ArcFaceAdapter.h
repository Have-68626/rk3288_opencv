#pragma once

#include "Embedder.h"
#include "ArcFaceEmbedder.h"

#include <mutex>
#include <string>

namespace rk_core {

class ArcFaceAdapter : public Embedder {
public:
    ArcFaceAdapter();
    ~ArcFaceAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    std::optional<std::vector<float>> embed(const cv::Mat& alignedFaceBgr, std::string& err) override;
    int embeddingDim() const override;
    const char* name() const override;

    // P1-1：注入 manifest 指定的输入/输出节点名（真实模型如 in0/out0）。
    // ncnn 后端用节点名（历史默认 data/output）；OpenCV DNN 后端用 blob 名（历史默认空=自动），
    // 两者同源对齐。空串忽略以保留原默认；未调用时行为与历史完全一致，旧调用方不受影响。
    // 须在 load() 前调用（load 只按扩展名选后端并填路径，不改节点名）。
    void setNodeNames(const std::string& input, const std::string& output);

    // 缺口2：注入 manifest 声明的 .bin 路径（与检测侧 loadDetector 显式消费 detectorBin 对称）。
    // 空串忽略 ⇒ 回落到历史推导（param stem 的 replace_extension(".bin")）。
    // 仅对 load() 的 **.param** 分支生效：同 stem 场景下注入路径与推导路径指向同一文件，
    // 故既有行为不变；名字与 param stem 不同时，才会加载 manifest 声明的那个 bin。
    // 须在 load() 前调用。
    void setModelBinPath(const std::string& binPath);

    // 当前生效的输入/输出节点名（诊断与测试用；未注入时为默认 data/output）。
    const std::string& inputNodeName() const;
    const std::string& outputNodeName() const;

    // 缺口3：可注入的期望嵌入维度。默认值 = ArcFaceEmbedding::kDim(512) ⇒ 未注入时
    // embeddingDim() 与历史逐字一致，生产行为不变。注入的是 manifest **声明值**；模型实际输出
    // 仍可能与之不符（.param 与 manifest 漂移），故 RealtimeFacePipeline 的 selfCheck 仍对
    // **实际** emb.size() 做事后比对，不信任此声明值（见 RealtimeFacePipeline.cpp selfCheck）。
    // 非法值（<=0）忽略，保留既有期望值。
    void setExpectedEmbeddingDim(int dim);

private:
    mutable std::mutex mu_;
    ArcFaceEmbedder inner_;
    ArcFaceEmbedderConfig cfg_;
    bool loaded_ = false;
    std::string currentName_;
    // 期望嵌入维度（缺口3）。默认 kDim；仅经 setExpectedEmbeddingDim 修改。
    int expectedDim_ = ArcFaceEmbedding::kDim;
    // 注入的 .bin 路径（缺口2）。空串 ⇒ load() 回落到按 param stem 推导。
    std::string injectedBinPath_;
};

} // namespace rk_core
