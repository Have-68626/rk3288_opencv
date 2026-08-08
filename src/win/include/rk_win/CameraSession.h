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
    // 相机会话 API 占位：switchWithRollback 已随 FramePipeline 内部切换实现退役
    // （2026-08 低关联清理 B06-016）；CameraDeviceInfo/CameraResult/CameraOpenParams 保留作公开类型
};

}  // namespace rk_win
