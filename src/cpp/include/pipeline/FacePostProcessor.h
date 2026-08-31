#pragma once
#include <vector>
#include <cstdint>

#include "pipeline/TrackCoordinator.h"  // pipeline::DetectedFace

namespace pipeline {

struct FacePostProcessOptions {
    float nmsIouThreshold = 0.40f;   // IoU NMS 阈值（计划固定值）
    float containmentRatio = 0.80f;  // 较小框被较大框包含的面积占比阈值
    int maxFaces = 0;                // 输出上限；0 = 不限
};

/**
 * 检测结果统一后处理（任务4）。
 *
 * 固定处理顺序：
 *   丢弃 NaN/负尺寸/越界后无有效面积的框 → 裁剪到画面边界 → 按置信度降序（同分数保持
 *   原始次序）稳定排序 → IoU NMS → 包含框抑制（较小框 ≥ containmentRatio 被较大框包含
 *   且分数不更高时删除较小框）→ maxFaces 截断。
 *
 * 纯几何 + 置信度，不依赖完整 OpenCV（仅 cv::Rect，供 core_unit_tests 无 OpenCV 构建）。
 */
class FacePostProcessor {
public:
    static std::vector<DetectedFace> process(
        std::vector<DetectedFace> faces,
        int frameWidth, int frameHeight,
        const FacePostProcessOptions& opt = FacePostProcessOptions{});

    /**
     * 与 process 相同的过滤流程，返回保留检测在 faces 原始顺序中的索引
     * （按最终输出顺序排列）。供需要同步附加数据（如关键点）的调用方
     * 将过滤索引映射回原始检测数组，避免二次解析。
     */
    static std::vector<std::size_t> keptIndices(
        const std::vector<DetectedFace>& faces,
        int frameWidth, int frameHeight,
        const FacePostProcessOptions& opt = FacePostProcessOptions{});
};

}  // namespace pipeline