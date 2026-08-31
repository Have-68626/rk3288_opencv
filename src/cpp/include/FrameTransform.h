/**
 * @file FrameTransform.h
 * @brief 每帧图像变换的纯配置组件（RK3288 翻转抖动修复的任务 1 语义固化）。
 *
 * 变换完全由 FrameTransformConfig 决定，不读取检测框、轨迹或人物位置；
 * 像素、检测框、关键点统一使用同一变换，且每帧最多应用一次。
 *
 * 水平实际变换 = input_mirrored_x XOR target_mirrored_x：
 * - input_mirrored_x：输入源是否已镜像（相机元数据，如前置传感器输出）；
 * - target_mirrored_x：目标显示是否镜像（用户偏好，按 cameraId 持久化）。
 * generation 仅在相机或变换配置变化时递增，不能由检测结果改变。
 */
#pragma once

#include <opencv2/core.hpp>
#include <cstdint>

namespace rk_core {

struct FrameTransformConfig {
    int rotation_degrees = 0;      // 0/90/180/270
    bool input_mirrored_x = false; // 输入源物理已镜像
    bool target_mirrored_x = false;// 目标显示是否镜像（用户偏好）
    bool flip_y = false;           // 目标垂直翻转（用户偏好）
    uint64_t generation = 0;       // 仅配置变化递增，不由检测改变
};

class FrameTransform {
public:
    // 实际水平是否翻转（input XOR target）
    static bool effectiveFlipX(const FrameTransformConfig& c);

    // 配置是否完全相同（用于判定 generation 是否应变更；generation 本身不计入）
    static bool configUnchanged(const FrameTransformConfig& a, const FrameTransformConfig& b);

    // 应用完整变换（先旋转后翻转）。输入 frame 不被修改；无实际变换时返回输入引用（零拷贝）。
    static cv::Mat applyImage(const cv::Mat& frame, const FrameTransformConfig& c);

    // 将输入坐标系的点/矩形映射到 applyImage 输出坐标系（像素、框、关键点同一变换）。
    // inWidth/inHeight 为输入图像（变换前）的尺寸。
    static cv::Point2f mapPoint(const cv::Point2f& p, const FrameTransformConfig& c,
                                int inWidth, int inHeight);
    static cv::Rect mapRect(const cv::Rect& r, const FrameTransformConfig& c,
                            int inWidth, int inHeight);
};

}  // namespace rk_core