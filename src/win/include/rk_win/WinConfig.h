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
    std::filesystem::path arcFaceModelPath;
    // ArcFace OpenCV-DNN 的输入/输出节点名（FACE-ARCWIN-001）。
    // **默认留空**——空串即走 OpenCV「自动默认输入 / 首个输出」兜底，这正是修复本身：
    // 此前该节点名被硬编码为 `data`/`output`（其实是 ArcFaceEmbedderConfig 的 ncnn 字段默认值），
    // 与真实 ONNX 图命名不符，导致推理期抛 `Requested blob "data" not found`（异常被吞 ⇒
    // identify() 恒返回 UNKNOWN）。勿再把 `data`/`output` 作为回退默认值引入。
    std::string arcFaceInputName;
    std::string arcFaceOutputName;
    int minFaceSizePx = 60;
    // **LBPH 专用**阈值：卡方距离（值域约 [0,64]），`FaceRecognizer::identify` 用「距离 <= 阈值」判定。
    // ⚠️ 勿把它当作通用阈值传给其它识别后端——历史上 ArcFace 分支曾逐字复用它做**余弦相似度**判定，
    //    导致 `bestSim(∈[-1,1]) >= 55.0` 恒假、识别恒返回 UNKNOWN（FACE-ARCWIN-006 的根因，
    //    即「一个字段被两个量纲不同的后端共用」）。ArcFace 请用下方 identifyThresholdArcFace。
    //    键名/默认值/语义一律保持不变：改名会让既有 config.json 的旧键在启动校验时被直接拒绝。
    double identifyThreshold = 55.0;
    // ArcFace 专用阈值：**余弦相似度**语义（值域 [-1,1]，取 [0,1] 为合法阈值区间）。
    // `ArcFaceWinRecognizer::identify` 用「bestSim >= 阈值」判定，与 identifyThreshold 的量纲无关。
    double identifyThresholdArcFace = 0.45;
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

