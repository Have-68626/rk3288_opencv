#include "AccelerationContract.h"
#include <gtest/gtest.h>

#include <string>

// Q-CQ-B07-003：预览几何缓存核心（JNI preview 共享纯逻辑，无 Android 依赖）
#include "../../src/cpp/jni/PreviewGeomCache.h"

namespace {

bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

}  // namespace

TEST(AccelerationContract, NormalizeKeyAndBackend) {
    using rk_accel::normalizeBackendName;
    using rk_accel::normalizeContractKey;

    EXPECT_EQ(normalizeContractKey("enable_opencl"), "opencl");
    EXPECT_EQ(normalizeContractKey("RK_USE_MPP"), "mpp");
    EXPECT_EQ(normalizeContractKey("enableQualcomm"), "qualcomm");
    EXPECT_EQ(normalizeContractKey("detector_backend"), "detector_backend");
    EXPECT_EQ(normalizeContractKey("recognitionBackend"), "recognition_backend");
    EXPECT_EQ(normalizeContractKey("unknown_key"), "unknown_key");

    EXPECT_EQ(normalizeBackendName("OpenCV"), "opencv_dnn");
    EXPECT_EQ(normalizeBackendName("opencv-dnn"), "opencv_dnn");
    EXPECT_EQ(normalizeBackendName("NCNN"), "ncnn");
    EXPECT_EQ(normalizeBackendName("Qualcomm"), "qualcomm");
    EXPECT_EQ(normalizeBackendName(""), "opencv_dnn");
}

TEST(AccelerationContract, FormatSelfCheckLine) {
    rk_accel::AccelContractStatus st;
    st.key = "opencl";
    st.requested = true;
    st.effective = false;
    st.reason = "missing_dependency";
    st.evidence = "haveOpenCL=0";

    const std::string line = rk_accel::formatSelfCheckLine(st);
    EXPECT_TRUE(contains(line, "ACCEL_SELF_CHECK"));
    EXPECT_TRUE(contains(line, "[path=opencl]"));
    EXPECT_TRUE(contains(line, "key=opencl"));
    EXPECT_TRUE(contains(line, "requested=1"));
    EXPECT_TRUE(contains(line, "effective=0"));
    EXPECT_TRUE(contains(line, "reason=missing_dependency"));
    EXPECT_TRUE(contains(line, "evidence=haveOpenCL=0"));
}

// ===== Q-CQ-B07-003：Surface 重建后预览几何缓存必须失效并重设 =====
// RED 判别逻辑：复现「Surface 重建先复现旧 geometry」——
//   1) 初始 Surface S1（代次 1）以尺寸 A 下发几何，缓存记录 A；
//   2) 模拟 Surface 重建为 S2（代次 2），帧尺寸仍为 A（相机分辨率不变）；
//   3) 断言 must resend（应为 TRUE）。
// 修复前旧实现是不感知代次的 (w,h,format) 静态缓存：同尺寸重建会被误判为
// 「已设置」而跳过 setBuffersGeometry，新 Surface 沿用旧 geometry，渲染守卫
// 持续失败 → EXPECT_TRUE 判红。修复后缓存以代次为第一失效维度 → 判绿。
TEST(PreviewGeomCache, SurfaceRebuildSameSizeMustResend) {
    rk_jni::PreviewGeomCache cache;
    const int fmt = 1; // WINDOW_FORMAT_RGBA_8888 占位（与尺寸无关）
    // 初始 Surface S1，代次 1，尺寸 A
    cache.update(1, 1280, 720, fmt);
    // 同代次同尺寸：几何已下发，无需重复下发
    EXPECT_FALSE(cache.shouldSetGeometry(1, 1280, 720, fmt));
    // 重建为 S2（代次 2）且帧尺寸仍为 A —— 旧缓存必须失效（RED 判别点）
    EXPECT_TRUE(cache.shouldSetGeometry(2, 1280, 720, fmt));
}

// 新 Surface 尺寸 B 必须生效：重建后不同尺寸立即触发下发，下发后同代次不重复
TEST(PreviewGeomCache, NewSurfaceSizeTakesEffect) {
    rk_jni::PreviewGeomCache cache;
    const int fmt = 1;
    cache.update(1, 1280, 720, fmt);
    // 重建为 S2（代次 2）且帧尺寸变为 B —— 必须重新下发
    EXPECT_TRUE(cache.shouldSetGeometry(2, 1920, 1080, fmt));
    cache.update(2, 1920, 1080, fmt);
    // 已下发：同代次同尺寸不再重复调用 setBuffersGeometry
    EXPECT_FALSE(cache.shouldSetGeometry(2, 1920, 1080, fmt));
}

// 同代次内格式变化仍需下发（守卫依赖 buffer->format == RGBA_8888）
TEST(PreviewGeomCache, FormatChangeWithinGeneration) {
    rk_jni::PreviewGeomCache cache;
    cache.update(1, 1280, 720, 1);
    EXPECT_TRUE(cache.shouldSetGeometry(1, 1280, 720, 2));
}
