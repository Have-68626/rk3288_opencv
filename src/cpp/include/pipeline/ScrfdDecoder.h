#pragma once
#include <string>
#include <vector>

#include "pipeline/FacePostProcessor.h"

namespace pipeline {

// SCRFD 三层输出张量视图（仅索引 + 形状 + 数据长度，数据由调用方持有）
struct ScrfdTensor {
    const float* data = nullptr;
    int channels = 0;
    int featW = 0;
    int featH = 0;
    std::size_t dataLen = 0;  // float 元素数；>0 时解码前校验越界读
    bool valid() const {
        return data != nullptr && channels > 0 && featW > 0 && featH > 0;
    }
};

struct Keypoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct ScrfdDetection {
    cv::Rect bbox;                       // 原图坐标（int，供下游追踪/渲染）
    float score = 0.0f;
    std::vector<Keypoint> keypoints;     // 5 点（原图坐标）
};

struct ScrfdDecodeOptions {
    int origWidth = 0;      // 原图尺寸（letterbox 映射目标）
    int origHeight = 0;
    // letterbox：640×640 输入坐标 → 原图坐标：x_orig = (x - padX) / scale
    float padX = 0.0f;
    float padY = 0.0f;
    float scale = 1.0f;
    int stride[3] = {8, 16, 32};       // 与 score/bbox/kps 张量一一对应
    float anchorScale[3] = {1.0f, 1.0f, 1.0f};  // anchor 基线尺寸 = stride×scale（SCRFD anchor；默认简化 1）
    float confThreshold = 0.55f;        // 检测置信度
    FacePostProcessOptions post;        // 转发 FacePostProcessor（NMS/包含抑制/maxFaces），默认 0.40/0.80
};

/**
 * 纯 SCRFD 解码器（任务6）：构造张量即可测，不依赖真实模型/OpenCV DNN。
 *
 * 约定：三层 score(=stride 8/16/32)、三段 bbox(channels=4)、三段 kps(channels=10)；
 * score 层 channels=2 取 [bg,fg] 的 fg，channels=1 取唯一值。
 * 模型加载时须用本解码器校验实际节点与张量形状；不匹配返回 false 并填 err，
 * 禁止猜测解码。
 */
class ScrfdDecoder {
public:
    static std::vector<ScrfdDetection> decode(
        const ScrfdTensor score[3], const ScrfdTensor bbox[3], const ScrfdTensor kps[3],
        const ScrfdDecodeOptions& opt, std::string& err);
};

}  // namespace pipeline