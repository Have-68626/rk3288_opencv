#pragma once

#ifdef _WIN32

#include "StructuredLogger.h"

#include <cstdint>
#include <memory>
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

// 相机采集源抽象：FramePipeline 通过此接口驱动采集。
// 便于测试注入假相机（记录 open 的设备、返回受控帧），验证"切换后采集源确实改变"。
class ICameraSource {
public:
    virtual ~ICameraSource() = default;
    virtual CameraOpenResult open(const std::wstring& deviceId, int width, int height, int fps) = 0;
    virtual void close() = 0;
    virtual CameraOpenResult readFrameBgr(cv::Mat& outBgr, std::uint64_t& outTimestamp100ns) = 0;
    virtual bool isOpen() const = 0;
    virtual CameraDevice currentDevice() const = 0;
    virtual CameraFormat currentFormat() const = 0;
};

class MfCamera : public ICameraSource {
public:
    MfCamera();
    ~MfCamera() override;

    MfCamera(const MfCamera&) = delete;
    MfCamera& operator=(const MfCamera&) = delete;

    static std::vector<CameraDevice> enumerateDevices();

    CameraOpenResult open(const std::wstring& deviceId, int width, int height, int fps) override;
    void close() override;

    bool isOpen() const override;
    CameraDevice currentDevice() const override;
    CameraFormat currentFormat() const override;

    CameraOpenResult readFrameBgr(cv::Mat& outBgr, std::uint64_t& outTimestamp100ns) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rk_win

#endif

