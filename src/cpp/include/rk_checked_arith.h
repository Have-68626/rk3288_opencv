/**
 * @file rk_checked_arith.h
 * @brief 32 位有符号乘法 checked 辅助（JNI camera/Engine 尺寸/偏移计算）
 *
 * S-SEC-B09-003：preview.cpp 的 stride*4、row*stride，Engine.cpp 的
 * stride/pixelStride 乘积等在 armeabi-v7a（32 位 int）下可能 int32 溢出，
 * 产生负偏移/回绕，导致越界读写。本头提供溢出前拒绝的纯函数，host 可直接
 * 单测（tests/cpp/test_checked_arith.cpp）。
 */
#pragma once

#include <cstdint>
#include <limits>

namespace rk_jni {

// 32 位有符号乘法 checked：结果超出 int32 范围时返回 false 且不写 out
// （受控失败，调用方不得使用 out 继续越界写）；成功时写 out 并返回 true。
// 用 long long（armeabi-v7a 上亦 8 字节）精确计算，避免依赖编译器 builtin
// 的平台差异。
inline bool mulS32Checked(int a, int b, int& out) {
    const long long product =
        static_cast<long long>(a) * static_cast<long long>(b);
    const long long kMin =
        static_cast<long long>(std::numeric_limits<int>::min());
    const long long kMax =
        static_cast<long long>(std::numeric_limits<int>::max());
    if (product < kMin || product > kMax) {
        return false;
    }
    out = static_cast<int>(product);
    return true;
}

}  // namespace rk_jni
