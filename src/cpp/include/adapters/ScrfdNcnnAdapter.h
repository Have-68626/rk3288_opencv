#pragma once

#include "FaceDetector.h"
#include "pipeline/FacePostProcessor.h"
#include "pipeline/ScrfdDecoder.h"

#include <array>
#include <mutex>
#include <string>
#include <vector>

namespace rk_core {

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
class ncnn_net_holder;  // 前向声明，隐藏 ncnn 头（RK_SKIP_OPENCV 构建无 ncnn）
#endif

/**
 * SCRFD 检测适配器配置（任务7）。
 *
 * 节点名来自模型 manifest/配置，不散落硬编码；未提供时用 PNNX 约定
 * （输入 in0，输出 out0..out8 按 score/bbox/kps × stride 8/16/32）。
 *
 * 输出契约（normalizeLevel 归一化为 canonical）：9 输出按 out0..2 score、
 * out3..5 bbox、out6..8 kps（PNNX 约定）；若模型为 SCRFD 原生命名
 * （bin 名 score_8/bbox_8/kps_8 等），经 kpsNames/scoreNames/bboxNames 显式装配。
 */
struct ScrfdNcnnConfig {
    std::string paramPath;
    std::string binPath;
    std::string inputName = "in0";
    std::string scoreNames[3] = {"out0", "out1", "out2"};
    std::string bboxNames[3] = {"out3", "out4", "out5"};
    std::string kpsNames[3] = {"out6", "out7", "out8"};
    int inputW = 640;
    int inputH = 640;
    int stride[3] = {8, 16, 32};
    float anchorScale[3] = {1.0f, 1.0f, 1.0f};  // delta 模式 anchor 尺寸基线（distance 模式不使用）
    float confThreshold = 0.55f;
    pipeline::FacePostProcessOptions post;  // NMS/包含抑制/maxFaces（默认 0.40/0.80）
    int threads = 1;
    // golden 实证（2026-09-03）：真实 SCRFD 为每位置多 anchor + distance 解码。
    // 默认 kDelta + anchorsPerPos=1 兼容既有；真实模型设为 kDistance + anchorsPerPos=2。
    pipeline::DecodeMode decodeMode = pipeline::DecodeMode::kDelta;
    int anchorsPerPos[3] = {1, 1, 1};  // 每位置 anchor 数（真实 det_10g/kps 为 2）
    enum class OutputLayout { kAuto = 0, kNChw = 1, kFlat = 2 };
    OutputLayout outputLayout = OutputLayout::kAuto;  // 默认按形状嗅探
};

/**
 * ncnn SCRFD 适配器（rk_core::FaceDetector 实现）。
 *
 * - 未编译 ncnn → load 返回 err="build_disabled"；
 * - 模型文件缺失 → err="missing_model"；
 * - 节点/张量验证失败 → err 非空（"ncnn_*" / "scrfd_*"），禁止猜测解码；
 * - 首次成功推理（forward 完成）后 effective=true（isEffective()）。
 * - 解码经 pipeline::ScrfdDecoder + 统一后处理，与模型无关。
 */
class ScrfdNcnnAdapter : public FaceDetector {
public:
    ScrfdNcnnAdapter();
    explicit ScrfdNcnnAdapter(ScrfdNcnnConfig cfg);
    ~ScrfdNcnnAdapter() override;  // 显式析构：ncnn_net_holder 为不完整类型的关键函数（A2 M1）

    bool load(const std::string& modelPath, std::string& err) override;
    FaceDetections detect(const cv::Mat& bgr, std::string& err) override;
    const char* name() const override;

    bool isEffective() const { return effective_; }

    // 由 9 个输出张量（score/bbox/kps × 3）+ 解码选项生成检测（供测试注入模拟输出）
    static FaceDetections decodeFromTensors(
        const pipeline::ScrfdTensor score[3], const pipeline::ScrfdTensor bbox[3],
        const pipeline::ScrfdTensor kps[3], const pipeline::ScrfdDecodeOptions& opt,
        std::string& err);

    // 由配置构造解码选项（含 letterbox pad/scale 与原图尺寸）
    static pipeline::ScrfdDecodeOptions makeDecodeOptions(
        int origWidth, int origHeight, float padX, float padY, float scale,
        const ScrfdNcnnConfig& cfg);

    // 布局归一化：把单层 ncnn 输出（score 或 bbox 或 kps）× stride 的原始 shape
    // （flat 1D 或 NCHW）转为 canonical ScrfdTensor（每位置 N anchor 行连续；见备注契约）。
    //   - perPosCh：该层每 anchor 通道数（score=1、bbox=4、kps=10）
    //   - N：该层每位置 anchor 数（cfg.anchorsPerPos[l]）
    //   - featureSide：该层特征边长（inputW/stride）
    // 原始 shape：
    //   - kFlat（det_10g）：channels==1、data 为 (N*side²) 行 × perPosCh 列 → 零拷贝隐式改 shape
    //   - kNChw（kps-opt2）：channels==N*perPosCh、h==w==featureSide → 数据 ch-major，需拷贝重排
    // 成功返回 true 并填充 out；失败填 err。
    static bool normalizeLevel(
        int channels, int h, int w, const float* data, std::size_t dataLen,
        int perPosCh, int N, int featureSide, const ScrfdNcnnConfig::OutputLayout layout,
        std::vector<float>& scratch, pipeline::ScrfdTensor& out, std::string& err);

private:
    mutable std::mutex mu_;
    ScrfdNcnnConfig cfg_;
    bool loaded_ = false;
    bool effective_ = false;
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    std::unique_ptr<ncnn_net_holder> ncnn_;
#endif
};

}  // namespace rk_core