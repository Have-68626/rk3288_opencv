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
    if (!warn.empty()) events.append("config_init_warn", warn);

    rk_win::FramePipeline pipe;
    pipe.setEventLogger(&events);
    cv::ocl::setUseOpenCL(cfg.acceleration.enableOpenCL);
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

    while (rk_win::g_running) {
        bool applied = false;
        std::string err;
        if (settings.pollReloadOnce(applied, err) && applied) {
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

            const bool cameraChanged =
                (next.camera.preferredDeviceId != cfg.camera.preferredDeviceId) ||
                (next.camera.width != cfg.camera.width) ||
                (next.camera.height != cfg.camera.height) ||
                (next.camera.fps != cfg.camera.fps);
            if (cameraChanged) {
                rk_win::ensureCameraRunning(pipe, next, events);
            }

            const bool accelChanged = 
                (next.acceleration.enableOpenCL != cfg.acceleration.enableOpenCL) ||
                (next.acceleration.enableMpp != cfg.acceleration.enableMpp) ||
                (next.acceleration.enableQualcomm != cfg.acceleration.enableQualcomm);
            
            if (accelChanged) {
                events.append("acceleration_apply", "Restarting pipeline for acceleration changes");
                cv::ocl::setUseOpenCL(next.acceleration.enableOpenCL);
                // "Hot restart" pipeline
                pipe.shutdown();
                pipe.initialize(next);
                pipe.setPreviewLayout(1280, 720, next.ui.previewScaleMode);
                rk_win::ensureCameraRunning(pipe, next, events);
            } else if (!cameraChanged && (
                next.dnn.enable != cfg.dnn.enable ||
                next.dnn.modelPath != cfg.dnn.modelPath ||
                next.recognition.cascadePath != cfg.recognition.cascadePath ||
                next.recognition.databasePath != cfg.recognition.databasePath
            )) {
                // Config changed, need pipeline re-init
                events.append("pipeline_apply", "Restarting pipeline for model changes");
                pipe.shutdown();
                pipe.initialize(next);
                pipe.setPreviewLayout(1280, 720, next.ui.previewScaleMode);
                rk_win::ensureCameraRunning(pipe, next, events);
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

