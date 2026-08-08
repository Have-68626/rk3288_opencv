#include "rk_win/EventLogger.h"
#include "rk_win/FramePipeline.h"
#include "rk_win/HttpFacesPoster.h"
#include "rk_win/HttpFacesServer.h"
#include "rk_win/WinJsonConfig.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

// OpenCL 守卫：仅当构建期实际启用 OpenCL（HAVE_OPENCL 宏，由 opencv config 定义）
// 且 ocl.hpp 头存在时才 include 并调用 setUseOpenCL；否则编译期跳过，
// 规避 build_win 下 cv::ocl 未编译导致 C2039 的问题。
#if defined(HAVE_OPENCL) && defined(__has_include)
#  if __has_include(<opencv2/core/ocl.hpp>)
#    include <opencv2/core/ocl.hpp>
#  endif
#endif

namespace rk_win {
namespace {

std::atomic<bool> g_running{true};

#ifdef _WIN32
BOOL WINAPI consoleCtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_running = false;
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

int findDeviceIndexById(const std::vector<CameraDevice>& devices, const std::wstring& preferredId) {
    if (devices.empty()) return -1;
    if (!preferredId.empty()) {
        for (int i = 0; i < static_cast<int>(devices.size()); i++) {
            if (devices[i].deviceId == preferredId) return i;
        }
    }
    return 0;
}

void ensureCameraRunning(FramePipeline& pipe, const AppConfig& cfg, EventLogger& events) {
    const auto devs = pipe.devices();
    const int deviceIndex = findDeviceIndexById(devs, cfg.camera.preferredDeviceId);
    if (deviceIndex < 0) {
        events.append("camera_no_device", "no_camera_devices");
        return;
    }
    const auto r = pipe.applyCameraSettings(deviceIndex, cfg.camera.width, cfg.camera.height, cfg.camera.fps);
    if (!r.ok) {
        events.append("camera_apply_failed", r.reason);
    } else if (r.rolledBack) {
        events.append("camera_apply_rollback", r.reason);
    } else {
        events.append("camera_apply_ok",
                      "w=" + std::to_string(r.width) + " h=" + std::to_string(r.height) + " fps=" + std::to_string(r.fps));
    }
}

// ── ReloadPolicy 热更新 ──
enum class ChangeKind { None, Camera, Model, Preview, FullRestart };

ChangeKind classifyChange(const AppConfig& prev, const AppConfig& next) {
    // 加速参数变化需要整管线重启
    if (prev.acceleration.enableOpenCL != next.acceleration.enableOpenCL ||
        prev.acceleration.enableMpp != next.acceleration.enableMpp ||
        prev.acceleration.enableQualcomm != next.acceleration.enableQualcomm) {
        return ChangeKind::FullRestart;
    }
    // 相机参数变化（仅切换摄像头，不重建模型）
    if (prev.camera.preferredDeviceId != next.camera.preferredDeviceId ||
        prev.camera.width != next.camera.width ||
        prev.camera.height != next.camera.height ||
        prev.camera.fps != next.camera.fps) {
        return ChangeKind::Camera;
    }
    // 模型参数变化（重建 DNN + Recognizer）
    if (prev.model.recognition != next.model.recognition ||
        prev.dnn.enable != next.dnn.enable ||
        prev.dnn.modelPath != next.dnn.modelPath ||
        prev.recognition.cascadePath != next.recognition.cascadePath ||
        prev.recognition.databasePath != next.recognition.databasePath) {
        return ChangeKind::Model;
    }
    // 预览/UI 参数变化（仅更新布局）
    if (prev.ui.previewScaleMode != next.ui.previewScaleMode) {
        return ChangeKind::Preview;
    }
    return ChangeKind::None;
}

}  // namespace
}  // namespace rk_win

int main() {
#ifdef _WIN32
    SetConsoleCtrlHandler(rk_win::consoleCtrlHandler, TRUE);
#endif

    rk_win::WinJsonConfigStore settings;
    std::string warn;
    settings.initialize(warn);
    settings.startWatching();

    rk_win::AppConfig cfg = settings.current();

    rk_win::EventLogger events;
    events.open(cfg.log.logDir);

#ifndef BUILD_ID
#define BUILD_ID "unknown-dev"
#endif
    events.append("app_start", std::string("version=") + BUILD_ID);

    if (!warn.empty()) events.append("config_init_warn", warn);

    rk_win::FramePipeline pipe;
    pipe.setEventLogger(&events);
#if defined(HAVE_OPENCL) && defined(__has_include)
#  if __has_include(<opencv2/core/ocl.hpp>)
    cv::ocl::setUseOpenCL(cfg.acceleration.enableOpenCL);
#  endif
#endif
    pipe.initialize(cfg);
    pipe.setPreviewLayout(1280, 720, cfg.ui.previewScaleMode);
    rk_win::ensureCameraRunning(pipe, cfg, events);

    rk_win::HttpFacesServer http;
    if (cfg.http.enable) {
        http.start(&pipe, &events, cfg.http.port, &settings);
    }

    rk_win::HttpFacesPoster poster;
    if (cfg.poster.enable) {
        poster.start(&pipe, &events, cfg.poster.postUrl, cfg.poster.throttleMs, cfg.poster.backoffMinMs, cfg.poster.backoffMaxMs);
    }

    std::uint64_t lastAppliedGen = 0;  // 已消费的配置代数（PUT 或外部写入，恰好消费一次）
    while (rk_win::g_running) {
        bool applied = false;
        std::string err;
        // 驱动外部写入热重载（含校验/回滚）；PUT 落盘会自更新 lastWriteTime_，
        // pollReloadOnce 无法据此感知，消费统一由 generation 判定（两来源各一次、无重复/遗漏）
        settings.pollReloadOnce(applied, err);
        const std::uint64_t curGen = settings.generation();
        if (curGen != lastAppliedGen) {
            lastAppliedGen = curGen;
            rk_win::AppConfig next = settings.current();

            if (next.http.enable != cfg.http.enable || next.http.port != cfg.http.port) {
                http.stop();
                if (next.http.enable) http.start(&pipe, &events, next.http.port, &settings);
            }

            const bool posterChanged =
                (next.poster.enable != cfg.poster.enable) ||
                (next.poster.postUrl != cfg.poster.postUrl) ||
                (next.poster.throttleMs != cfg.poster.throttleMs) ||
                (next.poster.backoffMinMs != cfg.poster.backoffMinMs) ||
                (next.poster.backoffMaxMs != cfg.poster.backoffMaxMs);
            if (posterChanged) {
                poster.stop();
                if (next.poster.enable) {
                    poster.start(&pipe, &events, next.poster.postUrl, next.poster.throttleMs, next.poster.backoffMinMs, next.poster.backoffMaxMs);
                }
            }

            // ── ReloadPolicy：按变更类型局部热更新 ──
            auto kind = rk_win::classifyChange(cfg, next);
            switch (kind) {
            case rk_win::ChangeKind::Camera:
                pipe.switchCamera(next);
                break;
            case rk_win::ChangeKind::Model:
                pipe.reloadRuntime(next);
                break;
            case rk_win::ChangeKind::Preview:
                pipe.updatePreviewLayout(next.ui.windowWidth, next.ui.windowHeight,
                    std::to_string(next.ui.previewScaleMode));
                break;
            case rk_win::ChangeKind::FullRestart:
#if defined(HAVE_OPENCL) && defined(__has_include)
#  if __has_include(<opencv2/core/ocl.hpp>)
                cv::ocl::setUseOpenCL(next.acceleration.enableOpenCL);
#  endif
#endif
                pipe.shutdown();
                pipe.initialize(next);
                pipe.setPreviewLayout(1280, 720, next.ui.previewScaleMode);
                rk_win::ensureCameraRunning(pipe, next, events);
                break;
            case rk_win::ChangeKind::None:
            default:
                break;
            }

            cfg = std::move(next);
        } else if (!err.empty()) {
            events.append("config_reload_error", err);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    poster.stop();
    http.stop();
    pipe.shutdown();
    events.close();
    settings.stopWatching();
    return 0;
}

