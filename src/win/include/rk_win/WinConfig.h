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
    // 本服务的人脸接口只接受 GET；推送必须显式配置接收端。
    std::string postUrl;
    int throttleMs = 100;
    int backoffMinMs = 200;
    int backoffMaxMs = 5000;
};

struct LogConfig {
    std::filesystem::path logDir;
    // FACE-CONFIG-017：单份日志文件上限与轮转档数共同决定**单份文件集**的落盘量
    //   （= maxFileBytes × (maxRollFiles + 1)）；而落盘面**最多**为 4 份文件集 —— recognition.csv、
    //   recognition.jsonl（StructuredLogger）、event_log.jsonl（EventLogger **复用同一对上限、同一
    //   目录**，接线点见两个 main 的 events.open(...)）与 render_metrics.jsonl（RenderMetricsLogger，
    //   **仅**旧版 UI 目标 win_camera_face_recognition 持有）⇒ 最坏实占磁盘 = 上式 × 4
    //   （win_local_service 只写前 3 份；份数取 4 的推导见 kLogFileSetCount）。
    //   两者在解析/序列化两侧均经 clampLogMaxFileBytes / clampLogMaxRollFiles **只钳不拒**地压进
    //   总预算 512 MiB 的**均分额**（= kLogPerSetBudgetBytes = 512 MiB / 4 = 134217728 字节 = 128 MiB；
    //   默认 10 MiB × 6 = 60 MiB 每份，仅占均分额 46.9% ⇒ 健康装载零告警）。
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
    // 非空时以 manifest 装配 SCRFD/ArcFace，模板库按其已校验空间隔离；空值保留旧配置路径。
    std::filesystem::path faceProfileDir;
    std::string detection = "yolo_face";
    std::string recognition = "arcface";
    std::string backend = "opencv_dnn";
    std::string detectorBackend = "opencv_dnn";
    std::string recognitionBackend = "opencv_dnn";
    bool autoFallback = true;
    // 旧入口的 INT8 请求（faceProfileDir 为空时才消费）。profile 入口以 manifest.precision 为准。
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

// A18（FACE-CONFIG-017 补口）：从 INI/默认值装载配置。
// 为什么带可选出参：INI 首装 → JSON 落盘这条路径（WinJsonConfigStore::initialize 的 INI 分支）
//   此前**取不到** env-free 来源快照，只能把已套用 RK_WCFR_* 覆盖的 cfg 落盘 ⇒ 首装即把环境覆盖值
//   烤进 config.json（与 JSON 侧 FACE-CONFIG-017 的修法不对称）。补出参后，调用方可在**套用取值覆盖
//   之前**取到「纯 INI + 默认值 + 路径解析」的来源快照，供落盘侧使用；运行 current() 仍用 env-applied 值。
//   envFreeOut 非空时回填该来源快照；为 nullptr 时行为与旧签名逐字一致（向后兼容既有调用点）。
//   ⚠️ RK_WCFR_CONFIG 是**来源选择**（决定读哪个 INI/配置文件），不是取值覆盖 ⇒ 对快照与运行值同源、一并生效。
AppConfig loadConfigFromIniOrDefault(AppConfig* envFreeOut = nullptr);
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
// 落盘面**最多** 4 份文件集，各写 maxFileBytes × (maxRollFiles + 1) 字节：recognition.csv 与
//   recognition.jsonl（StructuredLogger）、event_log.jsonl（EventLogger —— 本批把它从硬编码
//   10 MiB / 5 改为随 cfg.log.maxFileBytes / maxRollFiles 接线，见两个 main 的 events.open(...)），
//   以及 render_metrics.jsonl（RenderMetricsLogger —— 本批为它接上轮转并传入同一对 cfg.log 上限，
//   **仅**旧版 UI 目标 win_camera_face_recognition 持有该日志器）；四份共用同一 logDir、同一对上限）
//   ⇒ 最坏实占磁盘 = 单份上界 × 4。
// 份数为何取 4（**取最大值上界**，勿退回按单一可执行体计数）：两个可执行体写入的文件集数不同 ——
//   win_local_service 写 recognition.csv + recognition.jsonl + event_log.jsonl = 3 份；
//   win_camera_face_recognition（旧版 UI，默认 OFF，经 -DRK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION_UI=ON
//   构建）在前 3 份之外**再加** render_metrics.jsonl：它持有 RenderMetricsLogger 成员并调
//   metrics.open(cfg.log.logDir, cfg.log.maxFileBytes, cfg.log.maxRollFiles) = 4 份。
//   而 cfg.log.maxFileBytes / maxRollFiles 是**同一份 config.json 的两个键**、被两个可执行体共用
//   ⇒ 预算要成立，就必须按**写日志最多**的那个进程（4 份）分配：预算只能取「最大值上界」，
//   取小会让旧版 UI 目标的最坏磁盘占用超出 512 MiB。保守方向安全 —— 按 4 分只会让实际占用更小。
// 预算取 512 MiB **总预算**（kLogBudgetBytes），四份均分 ⇒ 单份均分额 kLogPerSetBudgetBytes =
//   512 MiB / 4 = 134217728 字节（整除，余数 0），最坏实占磁盘 = 4 × 134217728 = 536870912 字节 =
//   512 MiB。改动前（无总预算钳制、schema 允许单份 1 GiB × 1001）最坏实占磁盘约 4 TiB
//   ⇒ 可通过配置造成磁盘耗尽。
// 总预算取 512 MiB 的理由：
//   · 与默认配置相容——默认每份 10 MiB × (5 + 1) = 60 MiB，仅占均分额 134217728 的 46.9%
//     ⇒ 既有默认部署**零告警**（「健康装载必须零告警」是硬约束，见 initialize 的 warnOut 语义）；
//   · 有明确上界且可解释——最坏实占磁盘 = 512 MiB（四份**合计**），相对改动前的最坏约 4 TiB；
//   · 单份预留 2.13× 余量，正常调大轮转档位的用户不会被误钳。
// maxFileBytes 的上界取**均分额的一半**：maxRollFiles 的 schema 下界是 1，即单份日志至少要占
//   maxFileBytes × 2；若只钳 maxRollFiles，maxFileBytes 取 schema 允许的 1 GiB 时乘积下界已是
//   2 GiB > 均分额 ⇒ 均分额无法成立。故必须同时给 maxFileBytes 留出「×2 后仍不超均分额」的空间。
// **只压上界、不抬下界**：下界（1024 / 1）属既有 schema 契约，放宽会改变「既有非法配置被拒」的语义。
// ⚠️ 份数一旦变化（新增第 5 份日志 / 退回 3 份），**必须**同步改下文两个常量 —— 「预算口径漏算
//   一份」正是本项要修的原始缺陷形态（历史上漏的是 EventLogger 复用的那一份）。
inline constexpr std::uint64_t kLogBudgetBytes = 512ULL * 1024ULL * 1024ULL;
// 落盘面**最多**的文件集份数（按写日志最多的可执行体计）：recognition.csv + recognition.jsonl +
//   event_log.jsonl（两个可执行体都写）+ render_metrics.jsonl（**仅**旧版 UI 目标写）。
// ⚠️ **本次由 3 改为 4，属行为变更（程度轻，须知情）**：`render_metrics.jsonl` 在本次之前**完全没有
//   轮转**（故当时按 3 份记账并不「漏算」，而是那份日志压根不受本预算约束）。给它接上 `cfg.log`
//   的同名键之后它才成为受约束的第 4 份 ⇒ 除数必须 +1，否则「≤ 512 MiB」这条记账不成立。
//   代价是**生效上限收紧**（相对除数 3 时）：`kLogMaxFileBytesCap` 由 89478485 降为 67108864
//   （85.33 → 64 MiB）；默认 `maxFileBytes = 10 MiB` 下 `maxRollFiles` 的上界由 16 降为 11。
//   默认部署（10 MiB / 5）远在两位之下 ⇒ **零行为变化、零告警**；只有 `maxFileBytes > 64 MiB`
//   或 `maxRollFiles > 11` 的**既有配置**会在启动装载时被**只钳不拒**地收敛并产生一条告警
//   （`configStatus` 随之翻为 `degraded`）。运维侧的交待见 `docs/guides/windows-service/
//   deploy_and_rollback.md` 的「升级注意」。
inline constexpr std::uint64_t kLogFileSetCount = 4;
// 单份文件集的均分额 = 总预算 / 份数 = 536870912 / 4 = 134217728（整除，恒 ≤ 总预算）。
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

