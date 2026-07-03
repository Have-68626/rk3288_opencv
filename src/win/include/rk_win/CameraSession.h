#pragma once
#include <string>
#include <vector>

namespace rk_win {

// 前向声明 — MfCamera 定义在 src/win/include/rk_win/MfCamera.h
struct CameraDeviceInfo {
    int index = -1;
    std::string name;
    std::string devicePath;
};

struct CameraResult {
    bool ok = false;
    std::string code;       // "ok" | "open_failed" | "first_frame_timeout" | "rollback_ok"
    std::string message;
    CameraDeviceInfo device;
};

struct CameraOpenParams {
    int deviceIndex = 0;
    int width = 640;
    int height = 480;
    int fps = 30;
    int firstFrameTimeoutMs = 5000;
};

class CameraSession {
public:
    // 尝试打开新摄像头，失败自动回滚到 previousDevice
    // previousDevice.index < 0 表示首次打开（无需回滚）
    static CameraResult switchWithRollback(
        const CameraOpenParams& params,
        const CameraDeviceInfo& previousDevice);
};

}  // namespace rk_win
