#include "rk_win/WinConfig.h"
#include "AccelerationContract.h"

#ifdef _WIN32
#include <windows.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace rk_win {
namespace {

constexpr int kInferenceIntervalDefaultMs = 150;
constexpr int kInferenceIntervalMinMs = 80;
constexpr int kInferenceIntervalMaxMs = 500;

std::wstring readIniW(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
#ifdef _WIN32
    wchar_t out[4096];
    DWORD n = GetPrivateProfileStringW(section, key, def, out, static_cast<DWORD>(std::size(out)), iniPath.wstring().c_str());
    return std::wstring(out, out + n);
#else
    (void)iniPath;
    (void)section;
    (void)key;
    return def ? std::wstring(def) : L"";
#endif
}

int readIniInt(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, int def) {
#ifdef _WIN32
    return static_cast<int>(GetPrivateProfileIntW(section, key, def, iniPath.wstring().c_str()));
#else
    (void)iniPath;
    (void)section;
    (void)key;
    return def;
#endif
}

bool readIniBool(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, bool def) {
    return readIniInt(iniPath, section, key, def ? 1 : 0) != 0;
}

std::uint64_t readIniU64(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, std::uint64_t def) {
    const std::wstring s = readIniW(iniPath, section, key, L"");
    if (s.empty()) return def;
    try {
        return static_cast<std::uint64_t>(std::stoull(s));
    } catch (...) {
        return def;
    }
}

double readIniDouble(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, double def) {
    const std::wstring s = readIniW(iniPath, section, key, L"");
    if (s.empty()) return def;
    try {
        return std::stod(s);
    } catch (...) {
        return def;
    }
}

std::filesystem::path toPath(const std::wstring& ws) {
    if (ws.empty()) return {};
    return std::filesystem::path(ws);
}

bool writeIniW(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
#ifdef _WIN32
    return WritePrivateProfileStringW(section, key, value.c_str(), iniPath.wstring().c_str()) == TRUE;
#else
    (void)iniPath;
    (void)section;
    (void)key;
    (void)value;
    return false;
#endif
}

std::wstring toWStringInt(std::int64_t v) {
    return std::to_wstring(v);
}

std::wstring toWStringDouble(double v) {
    wchar_t buf[64];
#ifdef _WIN32
    swprintf_s(buf, L"%.6f", v);
#else
    std::swprintf(buf, 64, L"%.6f", v);
#endif
    return buf;
}

}  // namespace

// ─── 共享工具函数实现 ──────────────────────────────────────────

std::wstring getEnvW(const wchar_t* name) {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return L"";
    return std::wstring(buf, buf + n);
#else
    (void)name;
    return L"";
#endif
}

std::string utf8FromWide(const std::wstring& ws) {
    if (ws.empty()) return {};
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
#else
    return std::string(ws.begin(), ws.end());
#endif
}

std::wstring wideFromUtf8(const std::string& s) {
    if (s.empty()) return L"";
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

std::string asciiLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string normalizeInferenceThrottleMode(std::string s) {
    s = asciiLower(std::move(s));
    if (s == "auto" || s == "manual" || s == "off") return s;
    return "auto";
}

int clampInferenceIntervalMs(int v) {
    return std::clamp(v, 80, 500);
}

// ─── 路径解析 ─────────────────────────────────────────────────

std::filesystem::path getExeDir() {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::filesystem::current_path();
    std::filesystem::path p(path);
    return p.parent_path();
#else
    return std::filesystem::current_path();
#endif
}

std::filesystem::path resolvePathFromExeDir(const std::filesystem::path& p) {
    if (p.empty()) return p;
    if (p.is_absolute()) return p;
    return getExeDir() / p;
}

AppConfig loadConfigFromIniOrDefault() {
    AppConfig cfg;

    std::filesystem::path iniPath = L"config/windows_camera_face_recognition.ini";
    const std::wstring envPath = getEnvW(L"RK_WCFR_CONFIG");
    if (!envPath.empty()) iniPath = envPath;

    cfg.configPath = resolvePathFromExeDir(iniPath);

    cfg.camera.preferredDeviceId = readIniW(cfg.configPath, L"camera", L"preferred_device_id", L"");
    cfg.camera.width = readIniInt(cfg.configPath, L"camera", L"width", cfg.camera.width);
    cfg.camera.height = readIniInt(cfg.configPath, L"camera", L"height", cfg.camera.height);
    cfg.camera.fps = readIniInt(cfg.configPath, L"camera", L"fps", cfg.camera.fps);

    cfg.recognition.cascadePath = resolvePathFromExeDir(toPath(readIniW(cfg.configPath, L"recognition", L"cascade_path", L"assets/lbpcascade_frontalface.xml")));
    cfg.recognition.databasePath = resolvePathFromExeDir(toPath(readIniW(cfg.configPath, L"recognition", L"database_path", L"storage/win_face_db.yml")));
    cfg.recognition.minFaceSizePx = readIniInt(cfg.configPath, L"recognition", L"min_face_size_px", cfg.recognition.minFaceSizePx);
    cfg.recognition.identifyThreshold = readIniDouble(cfg.configPath, L"recognition", L"identify_threshold", cfg.recognition.identifyThreshold);
    cfg.recognition.enrollSamples = readIniInt(cfg.configPath, L"recognition", L"enroll_samples", cfg.recognition.enrollSamples);

    cfg.inference.throttleMode =
        normalizeInferenceThrottleMode(utf8FromWide(readIniW(cfg.configPath, L"inference", L"throttle_mode", L"auto")));
    cfg.inference.intervalMs =
        clampInferenceIntervalMs(readIniInt(cfg.configPath, L"inference", L"interval_ms", kInferenceIntervalDefaultMs));

    cfg.dnn.enable = readIniBool(cfg.configPath, L"dnn", L"enable", cfg.dnn.enable);
    {
        std::filesystem::path mp = toPath(readIniW(cfg.configPath, L"dnn", L"model_path", L"storage/models/opencv_face_detector_uint8.pb"));
        std::filesystem::path cp = toPath(readIniW(cfg.configPath, L"dnn", L"config_path", L"storage/models/opencv_face_detector.pbtxt"));
        const std::wstring envModel = getEnvW(L"RK_WCFR_DNN_MODEL");
        const std::wstring envConfig = getEnvW(L"RK_WCFR_DNN_CONFIG");
        if (!envModel.empty()) mp = envModel;
        if (!envConfig.empty()) cp = envConfig;
        cfg.dnn.modelPath = resolvePathFromExeDir(mp);
        cfg.dnn.configPath = resolvePathFromExeDir(cp);
    }
    cfg.dnn.inputWidth = readIniInt(cfg.configPath, L"dnn", L"input_width", cfg.dnn.inputWidth);
    cfg.dnn.inputHeight = readIniInt(cfg.configPath, L"dnn", L"input_height", cfg.dnn.inputHeight);
    cfg.dnn.scale = readIniDouble(cfg.configPath, L"dnn", L"scale", cfg.dnn.scale);
    cfg.dnn.meanB = readIniInt(cfg.configPath, L"dnn", L"mean_b", cfg.dnn.meanB);
    cfg.dnn.meanG = readIniInt(cfg.configPath, L"dnn", L"mean_g", cfg.dnn.meanG);
    cfg.dnn.meanR = readIniInt(cfg.configPath, L"dnn", L"mean_r", cfg.dnn.meanR);
    cfg.dnn.swapRB = readIniBool(cfg.configPath, L"dnn", L"swap_rb", cfg.dnn.swapRB);
    cfg.dnn.confThreshold = readIniDouble(cfg.configPath, L"dnn", L"conf_threshold", cfg.dnn.confThreshold);
    cfg.dnn.backend = readIniInt(cfg.configPath, L"dnn", L"backend", cfg.dnn.backend);
    cfg.dnn.target = readIniInt(cfg.configPath, L"dnn", L"target", cfg.dnn.target);

    cfg.http.enable = readIniBool(cfg.configPath, L"http", L"enable", cfg.http.enable);
    cfg.http.port = readIniInt(cfg.configPath, L"http", L"port", cfg.http.port);
    {
        const std::wstring envPort = getEnvW(L"RK_WCFR_HTTP_PORT");
        if (!envPort.empty()) {
            try {
                const int p = std::stoi(envPort);
                if (p >= 1 && p <= 65535) cfg.http.port = p;
            } catch (...) {
            }
        }
    }

    cfg.poster.enable = readIniBool(cfg.configPath, L"poster", L"enable", cfg.poster.enable);
    cfg.poster.postUrl = utf8FromWide(readIniW(cfg.configPath, L"poster", L"post_url", L"http://localhost:8080/api/faces"));
    cfg.poster.throttleMs = readIniInt(cfg.configPath, L"poster", L"throttle_ms", cfg.poster.throttleMs);
    cfg.poster.backoffMinMs = readIniInt(cfg.configPath, L"poster", L"backoff_min_ms", cfg.poster.backoffMinMs);
    cfg.poster.backoffMaxMs = readIniInt(cfg.configPath, L"poster", L"backoff_max_ms", cfg.poster.backoffMaxMs);
    {
        const std::wstring envUrl = getEnvW(L"RK_WCFR_POST_URL");
        if (!envUrl.empty()) cfg.poster.postUrl = utf8FromWide(envUrl);
    }

    cfg.log.logDir = resolvePathFromExeDir(toPath(readIniW(cfg.configPath, L"log", L"log_dir", L"storage/win_logs")));
    cfg.log.maxFileBytes = readIniU64(cfg.configPath, L"log", L"max_file_bytes", cfg.log.maxFileBytes);
    cfg.log.maxRollFiles = readIniInt(cfg.configPath, L"log", L"max_roll_files", cfg.log.maxRollFiles);

    cfg.ui.windowWidth = readIniInt(cfg.configPath, L"ui", L"window_width", cfg.ui.windowWidth);
    cfg.ui.windowHeight = readIniInt(cfg.configPath, L"ui", L"window_height", cfg.ui.windowHeight);
    cfg.ui.previewScaleMode = readIniInt(cfg.configPath, L"ui", L"preview_scale_mode", cfg.ui.previewScaleMode);

    cfg.display.outputIndex = readIniInt(cfg.configPath, L"display", L"output_index", cfg.display.outputIndex);
    cfg.display.width = readIniInt(cfg.configPath, L"display", L"width", cfg.display.width);
    cfg.display.height = readIniInt(cfg.configPath, L"display", L"height", cfg.display.height);
    cfg.display.refreshNumerator = static_cast<std::uint32_t>(readIniInt(cfg.configPath, L"display", L"refresh_numerator", static_cast<int>(cfg.display.refreshNumerator)));
    cfg.display.refreshDenominator = static_cast<std::uint32_t>(readIniInt(cfg.configPath, L"display", L"refresh_denominator", static_cast<int>(cfg.display.refreshDenominator)));
    if (cfg.display.refreshDenominator == 0) cfg.display.refreshDenominator = 1;

    cfg.display.vsync = readIniBool(cfg.configPath, L"display", L"vsync", cfg.display.vsync);
    cfg.display.swapchainBuffers = readIniInt(cfg.configPath, L"display", L"swapchain_buffers", cfg.display.swapchainBuffers);
    cfg.display.fullscreen = readIniBool(cfg.configPath, L"display", L"fullscreen", cfg.display.fullscreen);
    cfg.display.allowSystemModeSwitch = readIniBool(cfg.configPath, L"display", L"allow_system_mode_switch", cfg.display.allowSystemModeSwitch);

    cfg.display.enableSRGB = readIniBool(cfg.configPath, L"display", L"enable_srgb", cfg.display.enableSRGB);
    cfg.display.gamma = readIniDouble(cfg.configPath, L"display", L"gamma", cfg.display.gamma);
    cfg.display.colorTempK = readIniInt(cfg.configPath, L"display", L"color_temp_k", cfg.display.colorTempK);

    cfg.display.aaSamples = readIniInt(cfg.configPath, L"display", L"aa_samples", cfg.display.aaSamples);
    cfg.display.anisoLevel = readIniInt(cfg.configPath, L"display", L"aniso_level", cfg.display.anisoLevel);

    cfg.acceleration.enableOpenCL = readIniBool(cfg.configPath, L"acceleration", L"enable_opencl", cfg.acceleration.enableOpenCL);
    cfg.acceleration.enableLibyuv = readIniBool(cfg.configPath, L"acceleration", L"enable_libyuv", cfg.acceleration.enableLibyuv);
    cfg.acceleration.enableMpp = readIniBool(cfg.configPath, L"acceleration", L"enable_mpp", cfg.acceleration.enableMpp);
    cfg.acceleration.enableQualcomm = readIniBool(cfg.configPath, L"acceleration", L"enable_qualcomm", cfg.acceleration.enableQualcomm);

    cfg.model.detection = utf8FromWide(readIniW(cfg.configPath, L"model", L"detection", wideFromUtf8(cfg.model.detection).c_str()));
    cfg.model.recognition = utf8FromWide(readIniW(cfg.configPath, L"model", L"recognition", wideFromUtf8(cfg.model.recognition).c_str()));
    cfg.model.backend = rk_accel::normalizeBackendName(
        utf8FromWide(readIniW(cfg.configPath, L"model", L"backend", wideFromUtf8(cfg.model.backend).c_str())),
        cfg.model.backend);
    cfg.model.detectorBackend = rk_accel::normalizeBackendName(
        utf8FromWide(readIniW(cfg.configPath, L"model", L"detector_backend", wideFromUtf8(cfg.model.backend).c_str())),
        cfg.model.backend);
    cfg.model.recognitionBackend = rk_accel::normalizeBackendName(
        utf8FromWide(readIniW(cfg.configPath, L"model", L"recognition_backend", wideFromUtf8(cfg.model.backend).c_str())),
        cfg.model.backend);
    cfg.model.autoFallback = readIniBool(cfg.configPath, L"model", L"auto_fallback", cfg.model.autoFallback);

    return cfg;
}

bool saveConfigToIni(const AppConfig& cfg) {
    if (cfg.configPath.empty()) return false;
    bool ok = true;

    ok = writeIniW(cfg.configPath, L"camera", L"preferred_device_id", cfg.camera.preferredDeviceId) && ok;
    ok = writeIniW(cfg.configPath, L"camera", L"width", toWStringInt(cfg.camera.width)) && ok;
    ok = writeIniW(cfg.configPath, L"camera", L"height", toWStringInt(cfg.camera.height)) && ok;
    ok = writeIniW(cfg.configPath, L"camera", L"fps", toWStringInt(cfg.camera.fps)) && ok;

    ok = writeIniW(cfg.configPath, L"recognition", L"cascade_path", cfg.recognition.cascadePath.wstring()) && ok;
    ok = writeIniW(cfg.configPath, L"recognition", L"database_path", cfg.recognition.databasePath.wstring()) && ok;
    ok = writeIniW(cfg.configPath, L"recognition", L"min_face_size_px", toWStringInt(cfg.recognition.minFaceSizePx)) && ok;
    ok = writeIniW(cfg.configPath, L"recognition", L"identify_threshold", toWStringDouble(cfg.recognition.identifyThreshold)) && ok;
    ok = writeIniW(cfg.configPath, L"recognition", L"enroll_samples", toWStringInt(cfg.recognition.enrollSamples)) && ok;

    ok = writeIniW(cfg.configPath, L"inference", L"throttle_mode", wideFromUtf8(normalizeInferenceThrottleMode(cfg.inference.throttleMode))) && ok;
    ok = writeIniW(cfg.configPath, L"inference", L"interval_ms", toWStringInt(clampInferenceIntervalMs(cfg.inference.intervalMs))) && ok;

    ok = writeIniW(cfg.configPath, L"dnn", L"enable", toWStringInt(cfg.dnn.enable ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"model_path", cfg.dnn.modelPath.wstring()) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"config_path", cfg.dnn.configPath.wstring()) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"input_width", toWStringInt(cfg.dnn.inputWidth)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"input_height", toWStringInt(cfg.dnn.inputHeight)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"scale", toWStringDouble(cfg.dnn.scale)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"mean_b", toWStringInt(cfg.dnn.meanB)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"mean_g", toWStringInt(cfg.dnn.meanG)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"mean_r", toWStringInt(cfg.dnn.meanR)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"swap_rb", toWStringInt(cfg.dnn.swapRB ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"conf_threshold", toWStringDouble(cfg.dnn.confThreshold)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"backend", toWStringInt(cfg.dnn.backend)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"target", toWStringInt(cfg.dnn.target)) && ok;

    ok = writeIniW(cfg.configPath, L"http", L"enable", toWStringInt(cfg.http.enable ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"http", L"port", toWStringInt(cfg.http.port)) && ok;

    ok = writeIniW(cfg.configPath, L"poster", L"enable", toWStringInt(cfg.poster.enable ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"poster", L"post_url", wideFromUtf8(cfg.poster.postUrl)) && ok;
    ok = writeIniW(cfg.configPath, L"poster", L"throttle_ms", toWStringInt(cfg.poster.throttleMs)) && ok;
    ok = writeIniW(cfg.configPath, L"poster", L"backoff_min_ms", toWStringInt(cfg.poster.backoffMinMs)) && ok;
    ok = writeIniW(cfg.configPath, L"poster", L"backoff_max_ms", toWStringInt(cfg.poster.backoffMaxMs)) && ok;

    ok = writeIniW(cfg.configPath, L"log", L"log_dir", cfg.log.logDir.wstring()) && ok;
    ok = writeIniW(cfg.configPath, L"log", L"max_file_bytes", toWStringInt(cfg.log.maxFileBytes)) && ok;
    ok = writeIniW(cfg.configPath, L"log", L"max_roll_files", toWStringInt(cfg.log.maxRollFiles)) && ok;

    ok = writeIniW(cfg.configPath, L"ui", L"window_width", toWStringInt(cfg.ui.windowWidth)) && ok;
    ok = writeIniW(cfg.configPath, L"ui", L"window_height", toWStringInt(cfg.ui.windowHeight)) && ok;
    ok = writeIniW(cfg.configPath, L"ui", L"preview_scale_mode", toWStringInt(cfg.ui.previewScaleMode)) && ok;

    ok = writeIniW(cfg.configPath, L"display", L"output_index", toWStringInt(cfg.display.outputIndex)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"width", toWStringInt(cfg.display.width)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"height", toWStringInt(cfg.display.height)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"refresh_numerator", toWStringInt(cfg.display.refreshNumerator)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"refresh_denominator", toWStringInt(cfg.display.refreshDenominator)) && ok;

    ok = writeIniW(cfg.configPath, L"display", L"vsync", toWStringInt(cfg.display.vsync ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"swapchain_buffers", toWStringInt(cfg.display.swapchainBuffers)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"fullscreen", toWStringInt(cfg.display.fullscreen ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"allow_system_mode_switch", toWStringInt(cfg.display.allowSystemModeSwitch ? 1 : 0)) && ok;

    ok = writeIniW(cfg.configPath, L"display", L"enable_srgb", toWStringInt(cfg.display.enableSRGB ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"gamma", toWStringDouble(cfg.display.gamma)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"color_temp_k", toWStringInt(cfg.display.colorTempK)) && ok;

    ok = writeIniW(cfg.configPath, L"display", L"aa_samples", toWStringInt(cfg.display.aaSamples)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"aniso_level", toWStringInt(cfg.display.anisoLevel)) && ok;

    ok = writeIniW(cfg.configPath, L"acceleration", L"enable_opencl", toWStringInt(cfg.acceleration.enableOpenCL ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"acceleration", L"enable_libyuv", toWStringInt(cfg.acceleration.enableLibyuv ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"acceleration", L"enable_mpp", toWStringInt(cfg.acceleration.enableMpp ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"acceleration", L"enable_qualcomm", toWStringInt(cfg.acceleration.enableQualcomm ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"detection", wideFromUtf8(cfg.model.detection)) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"recognition", wideFromUtf8(cfg.model.recognition)) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"backend", wideFromUtf8(rk_accel::normalizeBackendName(cfg.model.backend, "opencv_dnn"))) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"detector_backend", wideFromUtf8(rk_accel::normalizeBackendName(cfg.model.detectorBackend, "opencv_dnn"))) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"recognition_backend", wideFromUtf8(rk_accel::normalizeBackendName(cfg.model.recognitionBackend, "opencv_dnn"))) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"auto_fallback", toWStringInt(cfg.model.autoFallback ? 1 : 0)) && ok;

    return ok;
}

}  // namespace rk_win

