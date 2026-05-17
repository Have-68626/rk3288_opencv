/**
 * @file InferenceThrottle.h
 * @brief 推理节流（时间间隔）相关的模式与参数工具。
 *
 * 设计说明（为什么这样做）：
 * - 该头文件刻意不依赖 OpenCV/Engine，便于在 core_unit_tests（RK_SKIP_OPENCV=1）里做最小单测；
 * - 解析/钳制逻辑放在这里，避免 JNI/Engine/配置读写各自实现一套导致行为不一致。
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

enum class InferenceThrottleMode {
    Off = 0,
    Manual = 1,
    Auto = 2,
};

constexpr int kInferenceIntervalDefaultMs = 150;
constexpr int kInferenceIntervalMinMs = 80;
constexpr int kInferenceIntervalMaxMs = 500;

inline InferenceThrottleMode parseInferenceThrottleMode(const std::string& s) {
    std::string v;
    v.reserve(s.size());
    for (char c : s) {
        v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (v == "auto") return InferenceThrottleMode::Auto;
    if (v == "manual") return InferenceThrottleMode::Manual;
    return InferenceThrottleMode::Off;
}

inline int clampInferenceIntervalMs(int v) {
    return std::clamp(v, kInferenceIntervalMinMs, kInferenceIntervalMaxMs);
}

