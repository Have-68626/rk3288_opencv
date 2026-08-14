/**
 * @file jni/PreviewGeomCache.h
 * @brief 预览缓冲几何缓存核心（纯逻辑，无 Android/JNI 依赖，可单测）
 *
 * Q-CQ-B07-003：记录已通过 ANativeWindow_setBuffersGeometry 下发到 Surface
 * 的缓冲几何（代次 + 尺寸 + 格式），用于跳过重复下发。
 *
 * 关键语义：Surface 重建（g_previewGeneration 代次变化）后旧几何缓存必须
 * 失效并重设。否则新 Surface 会沿用旧 geometry——尺寸/格式守卫（stride、
 * format 校验）持续失败，预览卡黑。判定以「代次」为第一失效维度：即使帧
 * 尺寸与旧代次完全相同，代次变化也强制重新下发缓冲几何。
 */
#pragma once

#include <cstdint>

namespace rk_jni {

// 预览几何缓存：判定是否需要重新下发缓冲几何（纯函数核心）
struct PreviewGeomCache {
    uint64_t gen = 0;   // 上次下发几何对应的 Surface 代次
    int w = 0;          // 上次下发宽度（像素）
    int h = 0;          // 上次下发高度（像素）
    int format = 0;     // 上次下发缓冲格式（WINDOW_FORMAT_*）

    // 是否需要重新下发缓冲几何：Surface 代次变化 / 尺寸变化 / 格式变化任一成立
    bool shouldSetGeometry(uint64_t gen_, int w_, int h_, int format_) const {
        return gen_ != gen || w_ != w || h_ != h || format_ != format;
    }

    // 记录本次成功下发的几何（代次 + 尺寸 + 格式）
    void update(uint64_t gen_, int w_, int h_, int format_) {
        gen = gen_;
        w = w_;
        h = h_;
        format = format_;
    }
};

} // namespace rk_jni
