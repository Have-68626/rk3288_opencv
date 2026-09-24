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
    // **LBPH 专用**阈值：卡方距离，`FaceRecognizer::identify` 用「距离 <= 阈值」判定。
    // 合法区间 = [1, 64]（上界 64 是卡方距离的物理上界，推导见 clampIdentifyThreshold；下界 1 与
    //    三方 schema 的 minimum 同值）。钳制只发生在**上界侧**：`> 64` 的值在解析/序列化两侧均被
    //    clampIdentifyThreshold **只钳不拒**地就地修正为 64 并告警（启动期硬拒会触发 INI 回落清空
    //    用户配置）；`< 1` 的值**不钳** —— JSON 侧仍被 schema 的 `minimum: 1.0` 判**拒**（隔离坏配置
    //    + 默认值启动），只有 INI 读入路径会钳到 1。逐条射程见 clampIdentifyThreshold 的订正说明。
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
    // FACE-CONFIG-017：单份日志文件上限与轮转档数共同决定**单份文件集**的落盘量
    //   （= maxFileBytes × (maxRollFiles + 1)）；而本批的落盘面是 **3 份**文件集 —— recognition.csv、
    //   recognition.jsonl（StructuredLogger）与 event_log.jsonl（EventLogger **复用同一对上限、同一
    //   目录**，接线点见两个 main 的 events.open(...)）⇒ 实占磁盘 = 上式 × 3。
    //   两者在解析/序列化两侧均经 clampLogMaxFileBytes / clampLogMaxRollFiles **只钳不拒**地压进
    //   总预算 512 MiB 的**均分额**（= kLogPerSetBudgetBytes ≈ 170.67 MiB；默认 10 MiB × 6 = 60 MiB
    //   每份，仅占均分额 35.2% ⇒ 健康装载零告警）。
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
    // 启用 INT8 量化模型推理（请求语义）。
    // 消费点：`RuntimeBootstrap::build()`——**仅在 Windows 侧**记一条 `status="disabled"` 的
    // `int8_recognition` 快照（**恒可经 `/api/v1/models` 的 activeModels 观测**）并追加一条启动告警
    // （⚠️ 告警的可见性有条件：`FramePipeline` 只在 `!bootstrap.ok` 时才把它落到 `render_.status` /
    //  health 的 `warning`；识别器装配成功时这条文本不会上线，快照才是恒定的观测面）。
    // ⚠️ **如实口径**：Windows 运行时**没有**可用的 INT8 推理路径（`ArcFaceWinRecognizer` 把后端
    //    定死为 `OpenCvDnn`，而 INT8 产物是 ncnn 的 `.param`/`.bin`；`DnnSsdFaceDetector` 亦无 INT8
    //    入口）⇒ 置 true **不会**真的启用 INT8，只是把「该请求未被满足」变得可见。
    // ⚠️ 本键**不是** `rk_core::FaceInferRequest::int8Enabled` 的生产者：Windows 运行时
    //    （FramePipeline / RuntimeBootstrap）不构造 `FaceInferRequest`，核心 INT8 回落链的生产者是
    //    `rk3288_cli` 的 `--int8 1|0`（见 src/cpp/main.cpp 与 FaceInferencePipeline.h 的字段注释）。
    //    两者同名但走不同通路，**勿**据此推断「配了就走了核心 INT8 链」。
    // 默认 false ⇒ RuntimeBootstrap 不增减快照/告警，零行为变化。
    bool int8Enabled = false;
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

// ── 边界钳制与路径校验（FACE-ARCWIN-010 / FACE-CONFIG-017 / FACE-CONFIG-018）──
// 统一形态与 clampInferenceIntervalMs 一致：**只钳不拒**（路径校验入口本批亦只告警）。
// 为什么一律只钳不拒（承重理由，勿改成「拒绝」）：WinJsonConfigStore::initialize 在**校验失败**时
//   会走 INI 回落 + 写盘 ⇒ 清空用户全部配置。任何新增校验规则若让启动期
//   parseAndValidateSettingsDoc 返回 false，就是给用户造成配置丢失。故越界值一律
//   「warning + 就地修正」，只有 PUT（updateFromJsonBody）才允许拒绝（400）。

// FACE-ARCWIN-010（S-6）：LBPH 卡方距离阈值的物理上下界。
// 上界 64 的推导：LbphEmbedder 每 cell 归一化（Σ=1，共 gridX*gridY = 64 个 cell），
//   chiSquareDistance = 0.5·Σ(a−b)²/(a+b)；由 (a−b)² ≤ (a+b)² 得单 cell 项 ≤ 2 ⇒ 全局上界 64。
//   阈值 > 64 时 `distance <= thr` 恒真、阈值形同虚设。
// 下界 1.0 = 运行时 schema 的 minimum（JsonSchemaValidator.cpp），与 config.schema.json /
//   openapi.yaml 三方同值 ⇒ clamp 输出**恒**落在 schema 合法区间内，杜绝「钳完仍不合法 ⇒ 启动校验
//   被拒 ⇒ 清空配置」的死角。**不**取 0：取 0 就必须同步把三方 minimum 放宽到 0，而 docs/ 侧本批
//   已由另一代理收口、不在本次改动射程内（详见 WinConfig.cpp 定义处的取舍说明）。
// 这两个常量同时被 WinConfig.cpp 与 WinJsonConfig.cpp（规范化告警文本）使用 ⇒ 置于头文件。
inline constexpr double kIdentifyThresholdMin = 1.0;
inline constexpr double kIdentifyThresholdMax = 64.0;
double clampIdentifyThreshold(double v);

// FACE-CONFIG-017（S-3）：log 段写放大总预算。
// 落盘面 = **3 份**文件集，各写 maxFileBytes × (maxRollFiles + 1) 字节：recognition.csv 与
//   recognition.jsonl（StructuredLogger），以及 event_log.jsonl（EventLogger —— 本批把它从硬编码
//   10 MiB / 5 改为随 cfg.log.maxFileBytes / maxRollFiles 接线，见两个 main 的 events.open(...)；
//   三份共用同一 logDir、同一对上限）⇒ 最坏实占磁盘 = 单份上界 × 3。
// 预算取 512 MiB **总预算**（kLogBudgetBytes），三份均分 ⇒ 单份均分额 kLogPerSetBudgetBytes =
//   512 MiB / 3 = 178956970 字节（整数除法，余数 2 字节），最坏实占磁盘 = 3 × 178956970 =
//   536870910 字节 ≈ 512 MiB。改动前的上界（1 GiB × 1001 × 3 份）最坏约 3 TiB ⇒ 可通过配置造成磁盘耗尽。
// 总预算取 512 MiB 的理由：
//   · 与默认配置相容——默认每份 10 MiB × (5 + 1) = 60 MiB，仅占均分额 178956970 的 35.2%
//     ⇒ 既有默认部署**零告警**（「健康装载必须零告警」是硬约束，见 initialize 的 warnOut 语义）；
//   · 有明确上界且可解释——最坏实占磁盘 = 512 MiB（三份**合计**），相对改动前的最坏约 3 TiB；
//   · 单份预留 2.84× 余量，正常调大轮转档位的用户不会被误钳。
// maxFileBytes 的上界取**均分额的一半**：maxRollFiles 的 schema 下界是 1，即单份日志至少要占
//   maxFileBytes × 2；若只钳 maxRollFiles，maxFileBytes 取 schema 允许的 1 GiB 时乘积下界已是
//   2 GiB > 均分额 ⇒ 均分额无法成立。故必须同时给 maxFileBytes 留出「×2 后仍不超均分额」的空间。
// **只压上界、不抬下界**：下界（1024 / 1）属既有 schema 契约，放宽会改变「既有非法配置被拒」的语义。
// ⚠️ 份数一旦变化（新增第 4 份日志 / 退回 2 份），**必须**同步改下文两个常量 —— 「预算口径漏算
//   第 3 份」正是本项要修的原始缺陷形态（漏的是 EventLogger 复用的那一份）。
inline constexpr std::uint64_t kLogBudgetBytes = 512ULL * 1024ULL * 1024ULL;
// 落盘面的文件集份数：recognition.csv + recognition.jsonl + event_log.jsonl。
inline constexpr std::uint64_t kLogFileSetCount = 3;
// 单份文件集的均分额 = 总预算 / 份数 = 536870912 / 3 = 178956970（整数除法，恒 ≤ 总预算）。
inline constexpr std::uint64_t kLogPerSetBudgetBytes = kLogBudgetBytes / kLogFileSetCount;
inline constexpr std::uint64_t kLogMaxFileBytesCap = kLogPerSetBudgetBytes / 2;
std::uint64_t clampLogMaxFileBytes(std::uint64_t v);
int clampLogMaxRollFiles(std::uint64_t maxFileBytes, int v);

// FACE-CONFIG-018（S-1）：配置路径强化校验。
// Strict：调用方会对违规**拒绝**（本批仅 PUT 路径可用；启动期绝不可用——见上「只钳不拒」）；
// Warn  ：调用方只**告警**（启动期 / 热重载）。
// ⚠️ 两档的**判据完全相同**（只判 `..` 段），差别只在调用方的处置；绝对路径在两档下都**不**判违规
//   （绝对路径是本项目官方用法，理由见 WinConfig.cpp 定义处的取舍说明）。
// 返回 true = 通过（无违规）；false 时 reasonOut 说明原因。
enum class PathPolicy { Strict, Warn };
bool checkConfigPath(const std::filesystem::path& p, PathPolicy pol, std::string& reasonOut);

}  // namespace rk_win

