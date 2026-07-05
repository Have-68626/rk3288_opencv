#include "AccelerationContract.h"
#include <gtest/gtest.h>

#include <string>

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
