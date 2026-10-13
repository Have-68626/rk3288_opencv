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
 *
 * ⚠️ generation 的实际状况（如实标注，勿读成「已接线的配置代次」）：**生产代码既不写也不读它**。
 * Engine::preprocessFrame 构造 FrameTransformConfig 时只赋 input_mirrored_x / target_mirrored_x /
 * flip_y（rotation_degrees 用默认 0），**从不触碰 cfg.generation ⇒ 生产恒 0**；全仓对该字段的
 * 唯一触碰在单测 tests/cpp/test_frame_transform.cpp（赋一个非 0 值，验证 configUnchanged 排除它）。
 * Engine 层的等价计数器是 Engine::flipGeneration_（在 setFlip / setInputMirroredX 内递增、
 * 经 getFlipGeneration 读出），二者**无任何关联**。
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
    uint64_t generation = 0;       // ⚠️ 生产恒 0（Engine::preprocessFrame 构造时**不填充**本字段）；
                                   // 语义原为「仅配置变化递增，不由检测改变」，但**无生产写入方**。
                                   // Engine 层的等价计数器是 Engine::flipGeneration_，二者无关联。
};

class FrameTransform {
public:
    // 实际水平是否翻转（input XOR target）
    static bool effectiveFlipX(const FrameTransformConfig& c);

    // 配置是否完全相同（用于判定 generation 是否应变更；generation 本身不计入）
    // ⚠️ 但请勿据此推断存在 generation 消费方：生产侧无人填充、也无人读取该字段（见文件头 ⚠️），
    // 本函数在生产路径上无调用方，仅单测直接调用。
    static bool configUnchanged(const FrameTransformConfig& a, const FrameTransformConfig& b);

    // 应用完整变换（先旋转后翻转）。输入 frame 不被修改；无实际变换时返回输入引用（零拷贝）。
    static cv::Mat applyImage(const cv::Mat& frame, const FrameTransformConfig& c);

    // 将输入坐标系的点/矩形映射到 applyImage 输出坐标系（像素、框、关键点同一变换）。
    // inWidth/inHeight 为输入图像（变换前）的尺寸。
    // ⚠️ 本组函数**全仓无生产调用方**，仅单测直接调用 ⇒ 对它们的改动**没有生产路径
    //    回归保护**，须以用例为准；也不要因它们存在就推断生产会用到。
    //    生产帧路径不调用本组函数：**图像变换**走 applyImage（只做 cv::rotate / cv::flip，
    //    不含任何取整）；**检测框坐标的取整**在 Engine::postProcessRealtimeBoxes 内联实现
    //    （下界 floor / 上界 ceil，与其半开区间语义一致）。检测在变换后帧上执行、框天然处于
    //    同一坐标系，实时路径无需二次映射，故本组正向映射函数在其中始终不被触及。
    static cv::Point2f mapPoint(const cv::Point2f& p, const FrameTransformConfig& c,
                                int inWidth, int inHeight);
    static cv::Rect mapRect(const cv::Rect& r, const FrameTransformConfig& c,
                            int inWidth, int inHeight);
};

}  // namespace rk_core