#pragma once

// Windows Media Foundation 相机实现。接口与数据结构见 CameraSource.h（跨平台）。
// 非 Windows 平台不提供本类；请使用 createCameraSource()。

#include "rk_win/CameraSource.h"

#ifdef _WIN32

namespace rk_win {

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
