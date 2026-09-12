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

    // 当前生效的输入/输出节点名（诊断与测试用；未注入时为默认 data/output）。
    const std::string& inputNodeName() const;
    const std::string& outputNodeName() const;

private:
    mutable std::mutex mu_;
    ArcFaceEmbedder inner_;
    ArcFaceEmbedderConfig cfg_;
    bool loaded_ = false;
    std::string currentName_;
};

} // namespace rk_core
