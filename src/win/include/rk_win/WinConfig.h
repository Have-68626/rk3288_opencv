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
    bool enableMpp = false;
    bool enableQualcomm = false;
};

struct AppConfig {
    std::filesystem::path configPath;
    CameraConfig camera;
    RecognitionConfig recognition;
    DnnConfig dnn;
    HttpServerConfig http;
    HttpPosterConfig poster;
    LogConfig log;
    UiConfig ui;
    DisplayConfig display;
    AccelerationConfig acceleration;
};

AppConfig loadConfigFromIniOrDefault();
bool saveConfigToIni(const AppConfig& cfg);

std::filesystem::path resolvePathFromExeDir(const std::filesystem::path& p);
std::filesystem::path getExeDir();

}  // namespace rk_win

