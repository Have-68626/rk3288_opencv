#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace rk_accel {

struct AccelContractStatus {
    std::string key;
    bool requested = false;
    bool effective = false;
    std::string reason;
    std::string evidence;
};

inline std::string normalizeContractKey(std::string raw) {
    std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::replace(raw.begin(), raw.end(), '-', '_');

    if (raw == "rk_use_opencl" || raw == "enable_opencl" || raw == "enableopencl") return "opencl";
    if (raw == "rk_use_libyuv" || raw == "enable_libyuv" || raw == "enablelibyuv") return "libyuv";
    if (raw == "rk_use_mpp" || raw == "enable_mpp" || raw == "enablempp") return "mpp";
    if (raw == "rk_use_qualcomm" || raw == "enable_qualcomm" || raw == "enablequalcomm") return "qualcomm";
    if (raw == "detectorbackend" || raw == "detector_backend") return "detector_backend";
    if (raw == "recognitionbackend" || raw == "recognition_backend") return "recognition_backend";
    if (raw == "detectionthrottle" || raw == "detection_throttle") return "detection_throttle";
    if (raw == "recognitionthrottle" || raw == "recognition_throttle") return "recognition_throttle";
    return raw;
}

inline std::string normalizeBackendName(std::string raw, const std::string& fallback = "opencv_dnn") {
    std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::replace(raw.begin(), raw.end(), '-', '_');
    std::replace(raw.begin(), raw.end(), ' ', '_');

    if (raw.empty()) return fallback;
    if (raw == "opencv" || raw == "opencv_dnn") return "opencv_dnn";
    if (raw == "ncnn") return "ncnn";
    if (raw == "qualcomm") return "qualcomm";
    return fallback;
}

inline std::string normalizeReasonCode(std::string raw, bool requested, bool effective) {
    std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::replace(raw.begin(), raw.end(), '-', '_');
    std::replace(raw.begin(), raw.end(), ' ', '_');

    if (raw == "ok" ||
        raw == "build_disabled" ||
        raw == "unsupported_platform" ||
        raw == "missing_dependency" ||
        raw == "missing_model" ||
        raw == "runtime_init_failed" ||
        raw == "unsupported_input") {
        return raw;
    }

    if (effective) return "ok";
    if (!requested) return "build_disabled";
    return "runtime_init_failed";
}

inline std::string formatSelfCheckLine(const AccelContractStatus& status) {
    const std::string key = normalizeContractKey(status.key);
    const std::string reason = normalizeReasonCode(status.reason, status.requested, status.effective);
    return "ACCEL_SELF_CHECK [path=" + key + "] key=" + key +
           " requested=" + std::to_string(status.requested ? 1 : 0) +
           " effective=" + std::to_string(status.effective ? 1 : 0) +
           " reason=" + reason +
           " evidence=" + status.evidence;
}

}  // namespace rk_accel
