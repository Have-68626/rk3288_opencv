#pragma once

#include "Compat.h"

#include <cstdint>
#include <string>

namespace rk_win {

struct CameraConfig {
    std::wstring preferredDeviceId;
    int width = 640;
    int height = 480;
    int fps = 30;
};

struct RecognitionConfig {
    std::filesystem::path cascadePath;
    std::filesystem::path databasePath;
    int minFaceSizePx = 60;
    double identifyThreshold = 55.0;
    int enrollSamples = 12;
};

struct InferenceConfig {
    std::string throttleMode = "auto";
    int intervalMs = 150;
};

struct DnnConfig {
    bool enable = true;
    std::filesystem::path modelPath;
    std::filesystem::path configPath;
    int inputWidth = 300;
    int inputHeight = 300;
    double scale = 1.0;
    int meanB = 104;
    int meanG = 177;
    int meanR = 123;
    bool swapRB = false;
    double confThreshold = 0.50;
    int backend = 0;
    int target = 0;
};

struct HttpServerConfig {
    bool enable = true;
    int port = 8080;
};

struct HttpPosterConfig {
    bool enable = false;
    std::string postUrl = "http://localhost:8080/api/faces";
    int throttleMs = 100;
    int backoffMinMs = 200;
    int backoffMaxMs = 5000;
};

struct LogConfig {
    std::filesystem::path logDir;
    std::uint64_t maxFileBytes = 10ULL * 1024ULL * 1024ULL;
    int maxRollFiles = 5;
};

struct UiConfig {
    int windowWidth = 1280;
    int windowHeight = 800;
    int previewScaleMode = 0;
};

struct DisplayConfig {
    int outputIndex = 0;
    int width = 0;
    int height = 0;
    std::uint32_t refreshNumerator = 0;
    std::uint32_t refreshDenominator = 1;

    bool vsync = true;
    int swapchainBuffers = 2;
    bool fullscreen = false;
    bool allowSystemModeSwitch = false;

    bool enableSRGB = true;
    double gamma = 2.2;
    int colorTempK = 6500;

    int aaSamples = 1;
    int anisoLevel = 1;
};

struct AccelerationConfig {
    bool enableOpenCL = false;
    bool enableLibyuv = false;
    bool enableMpp = false;
    bool enableQualcomm = false;
};

struct ModelConfig {
    std::string detection = "yolo_face";
    std::string recognition = "arcface";
    std::string backend = "opencv_dnn";
    std::string detectorBackend = "opencv_dnn";
    std::string recognitionBackend = "opencv_dnn";
    bool autoFallback = true;
    bool int8Enabled = false;  // 启用 INT8 量化模型推理
};

struct AppConfig {
    std::filesystem::path configPath;
    CameraConfig camera;
    RecognitionConfig recognition;
    InferenceConfig inference;
    DnnConfig dnn;
    HttpServerConfig http;
    HttpPosterConfig poster;
    LogConfig log;
    UiConfig ui;
    DisplayConfig display;
    AccelerationConfig acceleration;
    ModelConfig model;
};

AppConfig loadConfigFromIniOrDefault();
bool saveConfigToIni(const AppConfig& cfg);

std::filesystem::path resolvePathFromExeDir(const std::filesystem::path& p);
std::filesystem::path getExeDir();

// ─── 共享工具函数 ────────────────────────────────────────────
// 以下函数供 WinConfig.cpp / WinJsonConfig.cpp 共同使用，避免实现漂移。

// 环境变量读取（Windows wide-char 版本）
std::wstring getEnvW(const wchar_t* name);
std::string utf8FromWide(const std::wstring& ws);
std::wstring wideFromUtf8(const std::string& s);

// 推理节流参数规范化
std::string asciiLower(std::string s);
std::string normalizeInferenceThrottleMode(std::string s);
int clampInferenceIntervalMs(int v);

}  // namespace rk_win

