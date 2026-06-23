#pragma once

#ifdef _WIN32

#include "StructuredLogger.h"

#include <cstdint>
#include <string>
#include <vector>

#ifndef RK_WIN_HAS_OPENCV
#if __has_include(<opencv2/core.hpp>)
#define RK_WIN_HAS_OPENCV 1
#else
#define RK_WIN_HAS_OPENCV 0
#endif
#endif

#if RK_WIN_HAS_OPENCV
#include <opencv2/core.hpp>
#else
namespace cv {
struct Mat;
}
#endif

namespace rk_win {

struct CameraFormat {
    int width = 0;
    int height = 0;
    int fps = 0;
    std::wstring subtype;
};

struct CameraDevice {
    std::wstring name;
    std::wstring deviceId;
    std::vector<CameraFormat> formats;
};

struct CameraOpenResult {
    bool ok = false;
    ErrorCategory category = ErrorCategory::Unknown;
    std::string code;
    std::string message;
};

class MfCamera {
public:
    MfCamera();
    ~MfCamera();

    MfCamera(const MfCamera&) = delete;
    MfCamera& operator=(const MfCamera&) = delete;

    static std::vector<CameraDevice> enumerateDevices();

    CameraOpenResult open(const std::wstring& deviceId, int width, int height, int fps);
    void close();

    bool isOpen() const;
    CameraDevice currentDevice() const;
    CameraFormat currentFormat() const;

    CameraOpenResult readFrameBgr(cv::Mat& outBgr, std::uint64_t& outTimestamp100ns);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace rk_win

#endif

