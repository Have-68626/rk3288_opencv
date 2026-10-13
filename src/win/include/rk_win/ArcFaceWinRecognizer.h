#pragma once

#include "FaceDatabase.h"
#include "FaceDetector.h"
#include "IRecognizer.h"

#include "Compat.h"
#include <ArcFaceEmbedder.h>

#include <string>
#include <vector>

namespace rk_win {

using rk_core::ArcFaceEmbedder;
using rk_core::ArcFaceEmbedderConfig;

class ArcFaceWinRecognizer : public IRecognizer {
public:
    bool initialize(const std::string& cascadePath, const std::filesystem::path& dbPath,
                    const std::string& arcFaceModelPath, int minFaceSizePx, double identifyThreshold);

    // 注入 OpenCV-DNN 的输入/输出节点名（FACE-ARCWIN-001）。
    // 语义：直接写入 embedderCfg_.opencvInput/opencvOutput；**空串 = 自动**（由 OpenCV 取
    // 默认输入 / 首个输出），此处**不**把空串替换成任何默认值。
    // ⚠️ 时序契约：必须在 initialize() **之前**调用——initialize 内部才把 cfg 交给 embedder_，
    //    其后调用无效。忘记调用**不会静默**：initialize 收尾阶段（`db_` 加载**之前**）有自检
    //    （合成图跑一次前向），节点名/模型异常时会 fail loud（打印 stderr 并返回 false）。
    // 注意：与 ArcFaceAdapter::setNodeNames 的「空串忽略」语义**不同**——此处空串即显式置空。
    void setNodeNames(std::string input, std::string output);

    std::vector<FaceMatch> identify(const cv::Mat& bgr) override;
    bool enrollFromFrame(const std::string& personId, const cv::Mat& bgr, int samplesToTake, int& samplesTakenOut) override;

    bool saveDb() const override;
    void clearDb() override;
    double threshold() const override;
    void setThreshold(double t) override;
    std::vector<std::string> personIds() const override;

private:
    static cv::Rect pickLargest(const std::vector<cv::Rect>& faces);
    static cv::Mat cropAndNormalizeFaceBgr(const cv::Mat& bgr, const cv::Rect& faceRect);

    static float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);
    static void l2NormalizeInplace(std::vector<float>& v);

    FaceDetector detector_;
    ArcFaceEmbedder embedder_;
    ArcFaceEmbedderConfig embedderCfg_;
    FaceDatabase db_;
    std::filesystem::path dbPath_;
    int minFaceSizePx_ = 60;
    // 成员默认：`initialize()` 一旦被调用即被覆盖（且该赋值已前置到函数最前，失败出口也能观测到注入
    // 值，见 ArcFaceWinRecognizer.cpp 的 FACE-ARCWIN-008 注释）⇒ 本默认值**实际只决定「构造后不调用
    // initialize()」这一不可达路径的读数**。**保留而非删除**：删掉会让未初始化的 double 成为 UB，比
    // 现状更危险。
    // ⚠️ 配置侧真源是 `WinConfig.h` 的 `RecognitionConfig::identifyThresholdArcFace`（同为 0.45）——
    //    两处数值相同、且此处不可观测，改其中一处不会提示另一处（FACE-ARCWIN-013 登记的事实）。
    double identifyThreshold_ = 0.45;  // 余弦相似度阈值（小于此值拒绝）
};

}  // namespace rk_win
