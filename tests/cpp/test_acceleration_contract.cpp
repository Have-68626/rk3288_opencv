#include "AccelerationContract.h"

#include <string>

namespace {

bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

}  // namespace

bool test_accel_contract_normalize_key_and_backend() {
    using rk_accel::normalizeBackendName;
    using rk_accel::normalizeContractKey;

    if (normalizeContractKey("enable_opencl") != "opencl") return false;
    if (normalizeContractKey("RK_USE_MPP") != "mpp") return false;
    if (normalizeContractKey("enableQualcomm") != "qualcomm") return false;
    if (normalizeContractKey("detector_backend") != "detector_backend") return false;
    if (normalizeContractKey("recognitionBackend") != "recognition_backend") return false;
    if (normalizeContractKey("unknown_key") != "unknown_key") return false;

    if (normalizeBackendName("OpenCV") != "opencv_dnn") return false;
    if (normalizeBackendName("opencv-dnn") != "opencv_dnn") return false;
    if (normalizeBackendName("NCNN") != "ncnn") return false;
    if (normalizeBackendName("Qualcomm") != "qualcomm") return false;
    if (normalizeBackendName("") != "opencv_dnn") return false;
    return true;
}

bool test_accel_contract_format_self_check_line() {
    rk_accel::AccelContractStatus st;
    st.key = "opencl";
    st.requested = true;
    st.effective = false;
    st.reason = "missing_dependency";
    st.evidence = "haveOpenCL=0";

    const std::string line = rk_accel::formatSelfCheckLine(st);
    if (!contains(line, "ACCEL_SELF_CHECK")) return false;
    if (!contains(line, "[path=opencl]")) return false;
    if (!contains(line, "key=opencl")) return false;
    if (!contains(line, "requested=1")) return false;
    if (!contains(line, "effective=0")) return false;
    if (!contains(line, "reason=missing_dependency")) return false;
    if (!contains(line, "evidence=haveOpenCL=0")) return false;
    return true;
}
