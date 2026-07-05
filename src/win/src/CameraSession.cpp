#include "rk_win/CameraSession.h"
#include "rk_win/MfCamera.h"
#include "rk_win/WinConfig.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rk_win {
namespace {

// 打开指定摄像头并等待首帧，成功时 result.ok=true
// 失败时返回具体错误码（open_failed / first_frame_timeout）
static CameraResult openAndWaitForFrame(
    const CameraOpenParams& params,
    int deviceIndex,
    const CameraDevice& device)
{
    CameraResult result;
    result.device.index = deviceIndex;
    result.device.name = utf8FromWide(device.name);
    result.device.devicePath = utf8FromWide(device.deviceId);

    MfCamera camera;
    CameraOpenResult openRes = camera.open(device.deviceId, params.width, params.height, params.fps);
    if (!openRes.ok) {
        result.code = "open_failed";
        result.message = "打开摄像头失败: " + openRes.code + " " + openRes.message;
        return result;
    }

    // 轮询等待首帧，超时即放弃
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(params.firstFrameTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        cv::Mat frame;
        std::uint64_t ts = 0;
        CameraOpenResult rr = camera.readFrameBgr(frame, ts);
        if (rr.ok && !frame.empty()) {
            result.ok = true;
            result.code = "ok";
            result.message = "ok";
            camera.close();
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    camera.close();

    result.code = "first_frame_timeout";
    result.message = "首帧超时（" + std::to_string(params.firstFrameTimeoutMs) + "ms）";
    return result;
}

// 尝试回滚到上一设备（打开 + 首帧验证）
// 仅在 previousDevice.index >= 0 且 switchWithRollback 中 target 失败时调用
static CameraResult rollbackToDevice(
    const CameraDeviceInfo& previousDevice,
    const CameraOpenParams& params)
{
    std::vector<CameraDevice> devices = MfCamera::enumerateDevices();

    if (previousDevice.index < 0 ||
        previousDevice.index >= static_cast<int>(devices.size())) {
        CameraResult rb;
        rb.code = "open_failed";
        rb.message = "回滚失败：上一设备索引 " + std::to_string(previousDevice.index) + " 已不可用";
        return rb;
    }

    const CameraDevice& prevDev = devices[static_cast<size_t>(previousDevice.index)];
    CameraResult rb = openAndWaitForFrame(params, previousDevice.index, prevDev);
    if (rb.ok) {
        rb.code = "rollback_ok";
        rb.message = "已回滚到上一设备: " + previousDevice.name;
    }
    return rb;
}

} // anonymous namespace

CameraResult CameraSession::switchWithRollback(
    const CameraOpenParams& params,
    const CameraDeviceInfo& previousDevice)
{
    // 1. 枚举可用设备
    std::vector<CameraDevice> devices = MfCamera::enumerateDevices();

    // 2. 校验目标设备索引
    if (params.deviceIndex < 0 ||
        params.deviceIndex >= static_cast<int>(devices.size())) {
        CameraResult result;
        result.code = "open_failed";
        result.message = "设备索引 " + std::to_string(params.deviceIndex)
                       + " 超出范围（共 " + std::to_string(devices.size()) + " 个设备）";

        if (previousDevice.index >= 0) {
            return rollbackToDevice(previousDevice, params);
        }
        return result;
    }

    // 3. 尝试打开目标摄像头并等待首帧
    const CameraDevice& targetDevice = devices[static_cast<size_t>(params.deviceIndex)];
    CameraResult result = openAndWaitForFrame(params, params.deviceIndex, targetDevice);

    if (result.ok) {
        return result; // code = "ok"
    }

    // 4. 失败 → 回滚到上一设备（若存在）
    if (previousDevice.index >= 0) {
        CameraResult rb = rollbackToDevice(previousDevice, params);
        if (rb.ok) {
            return rb; // code = "rollback_ok"
        }

        // 目标设备失败且回滚也失败 → 致命错误
        CameraResult fatal;
        fatal.code = "open_failed";
        fatal.message = "切换失败且回滚也失败（原设备可能已断开）: " + result.message
                      + "；回滚错误: " + rb.message;
        return fatal;
    }

    // 5. 首次打开（无上一设备）且失败 → 原样返回
    return result;
}

} // namespace rk_win
