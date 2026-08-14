// S-SEC-B09-003: 32 位 checked 乘法（JNI camera/Engine 尺寸/偏移计算）
//
// 负面用例：armeabi-v7a 上 int32 乘法在超大 stride/pixelStride/row 乘积时
// 溢出（变负/回绕），导致越界写/读。mulS32Checked 在溢出前返回 false（受控
// 失败，不写缓冲）；边界用例覆盖 INT_MAX/INT_MIN 及 46340² 边界。
// 结构测试验证接入点：preview.cpp/camera.cpp/Engine.cpp 的调用点已替换为
// checked 乘法（并移除裸 `* 4` / `row * dstStrideBytes` 表达式）。
#include "rk_checked_arith.h"
#include <gtest/gtest.h>

#include <climits>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// ── 纯函数边界用例：合法乘积精确返回 ───────────────────────────
TEST(MulS32Checked, InRangeProductsAreExact) {
    int out = 0;
    EXPECT_TRUE(rk_jni::mulS32Checked(0, INT_MAX, out));
    EXPECT_EQ(out, 0);
    EXPECT_TRUE(rk_jni::mulS32Checked(INT_MAX, 1, out));
    EXPECT_EQ(out, INT_MAX);
    EXPECT_TRUE(rk_jni::mulS32Checked(INT_MIN, 1, out));
    EXPECT_EQ(out, INT_MIN);
    EXPECT_TRUE(rk_jni::mulS32Checked(INT_MAX, -1, out));
    EXPECT_EQ(out, -INT_MAX);
    EXPECT_TRUE(rk_jni::mulS32Checked(-0x40000000, 2, out));  // -2^31 = INT_MIN
    EXPECT_EQ(out, INT_MIN);
    EXPECT_TRUE(rk_jni::mulS32Checked(46340, 46340, out));  // 2147395600 < INT_MAX
    EXPECT_EQ(out, 46340 * 46340);
    EXPECT_TRUE(rk_jni::mulS32Checked(1, 4, out));  // preview stride*4 常规值
    EXPECT_EQ(out, 4);
}

// ── 负面用例：溢出必须在计算前拒绝（受控失败，不写 out） ─────────
TEST(MulS32Checked, OverflowRejectedBeforeResult) {
    int out = 12345;  // 哨兵：失败时不得被写入
    EXPECT_FALSE(rk_jni::mulS32Checked(INT_MAX, 2, out));
    EXPECT_EQ(out, 12345);  // 溢出路径不写 out
    EXPECT_FALSE(rk_jni::mulS32Checked(INT_MIN, -1, out));
    EXPECT_FALSE(rk_jni::mulS32Checked(0x40000000, 2, out));  // 2^31 > INT_MAX
    EXPECT_FALSE(rk_jni::mulS32Checked(46341, 46341, out));   // 46341² 溢出
    EXPECT_FALSE(rk_jni::mulS32Checked(INT_MAX, INT_MAX, out));
    // preview.cpp 场景：超大 stride（driver 异常）时 stride*4 溢出
    EXPECT_FALSE(rk_jni::mulS32Checked(0x40000000, 4, out));
    // Engine.cpp 场景：超大 pixelStride * (w-1)（w≤1920）溢出
    EXPECT_FALSE(rk_jni::mulS32Checked(1919, 0x40000000, out));
}

// ── 结构测试：接入点验证（源文件必须已替换为 checked 乘法） ──────
static std::string readSource(const char* rel) {
    const std::string full = std::string(RK_TEST_SOURCE_ROOT) + "/" + rel;
    std::ifstream in(full, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

TEST(MulS32CheckedIntegration, PreviewCallSitesUseCheckedArith) {
    const std::string src = readSource("src/cpp/jni/preview.cpp");
    ASSERT_FALSE(src.empty()) << "无法读取 preview.cpp（RK_TEST_SOURCE_ROOT="
                              << RK_TEST_SOURCE_ROOT << "）";
    // 接入点：stride*4 与 row*stride 必须走 checked 乘法
    EXPECT_NE(src.find("rk_jni::mulS32Checked"), std::string::npos);
    // 负面结构：裸 int32 乘法表达式必须已移除
    EXPECT_EQ(src.find("dstStridePixels * 4"), std::string::npos);
    EXPECT_EQ(src.find("row * dstStrideBytes"), std::string::npos);
}

TEST(MulS32CheckedIntegration, CameraCallSitesCapPlaneBytes) {
    const std::string src = readSource("src/cpp/jni/camera.cpp");
    ASSERT_FALSE(src.empty()) << "无法读取 camera.cpp";
    // 接入点：平面 need 字节以 int64 精确计算 + 上限，32 位 size_t 不回绕
    EXPECT_NE(src.find("kMaxPlaneBytes"), std::string::npos);
    EXPECT_NE(src.find("int64_t"), std::string::npos);
}

TEST(MulS32CheckedIntegration, EngineCallSitesUseCheckedArith) {
    const std::string src = readSource("src/cpp/src/Engine.cpp");
    ASSERT_FALSE(src.empty()) << "无法读取 Engine.cpp";
    // 接入点：stride/pixelStride 乘积与缓冲末字节必须 checked/int64
    EXPECT_NE(src.find("rk_jni::mulS32Checked"), std::string::npos);
    EXPECT_NE(src.find("int64_t yLast"), std::string::npos);
    EXPECT_NE(src.find("int64_t uLast"), std::string::npos);
    EXPECT_NE(src.find("int64_t needY"), std::string::npos);
    // 负面结构：裸 int32 乘法比较表达式必须已移除（int64 计算中的同类子串除外）
    EXPECT_EQ(src.find("yRowStride < (w - 1) * yPixelStride + 1"), std::string::npos);
    EXPECT_EQ(src.find("uRowStride < (w2 - 1) * uPixelStride + 1"), std::string::npos);
    EXPECT_EQ(src.find("vRowStride < (w2 - 1) * vPixelStride + 1"), std::string::npos);
}

}  // namespace
