/**
 * @file ExternalFrame.h
 * @brief 外部帧输入：Native 侧的统一帧结构（供 JNI/Engine 使用）。
 *
 * 设计目标：
 * - 允许 Java/外部采集侧把 YUV_420_888 / NV21 帧直接喂入 core，旁路 VideoManager。
 * - 明确携带 stride 与旋转/镜像元数据，避免“看起来能跑但画面方向不对”的隐性问题。
 * - JNI 边界强调所有权：本结构默认由 Native 持有/拷贝数据，避免悬挂指针。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class ExternalFrameFormat : int32_t {
    YUV_420_888 = 0,
    NV21 = 1,
};

struct ExternalFramePlane {
    std::vector<uint8_t> bytes;
    int32_t rowStride = 0;
    int32_t pixelStride = 0;
};

struct ExternalFrameMetadata {
    int64_t timestampNs = 0;
    int32_t rotationDegrees = 0;
    bool mirrored = false;
};

struct ExternalFrame {
    ExternalFrameFormat format = ExternalFrameFormat::YUV_420_888;
    int32_t width = 0;
    int32_t height = 0;
    ExternalFrameMetadata meta{};

    ExternalFramePlane y{};
    ExternalFramePlane u{};
    ExternalFramePlane v{};

    std::vector<uint8_t> nv21;
    int32_t nv21RowStrideY = 0;

    std::string brief() const {
        const char* fmt = (format == ExternalFrameFormat::NV21) ? "NV21" : "YUV_420_888";
        return std::string(fmt) + " " + std::to_string(width) + "x" + std::to_string(height) +
               " tsNs=" + std::to_string(meta.timestampNs) +
               " rot=" + std::to_string(meta.rotationDegrees) +
               " mirror=" + (meta.mirrored ? "1" : "0");
    }
};

