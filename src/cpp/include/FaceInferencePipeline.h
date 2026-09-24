#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rk_core {

struct FaceInferRequest {
    std::string imagePath;

    std::string yoloBackend = "opencv";
    std::string yoloModelPath;
    std::string yoloConfigPath;
    std::string yoloFramework;
    std::string yoloOutputName;
    int yoloInputW = 320;
    int yoloInputH = 320;
    float yoloScoreThreshold = 0.25f;
    float yoloNmsIouThreshold = 0.45f;
    bool yoloEnableKeypoints5 = true;
    bool yoloLetterbox = true;
    bool yoloSwapRB = true;
    float yoloScale = 1.0f / 255.0f;
    int yoloMeanB = 0;
    int yoloMeanG = 0;
    int yoloMeanR = 0;
    int yoloOpenCvBackend = 0;
    int yoloOpenCvTarget = 0;

    std::string yoloNcnnParam;
    std::string yoloNcnnBin;
    std::string yoloNcnnInput = "data";
    std::string yoloNcnnOutput = "output";
    int yoloNcnnThreads = 1;
    bool yoloNcnnLightmode = true;

    std::string arcBackend = "opencv";
    std::string arcModelPath;
    std::string arcConfigPath;
    std::string arcFramework;
    std::string arcOutputName;
    std::string arcInputName;

    std::string arcNcnnParam;
    std::string arcNcnnBin;
    std::string arcNcnnInput = "data";
    std::string arcNcnnOutput = "output";
    int arcNcnnThreads = 1;
    bool arcNcnnLightmode = true;
    int arcInputW = 112;
    int arcInputH = 112;
    std::uint32_t arcModelVersion = 1;
    std::uint32_t arcPreprocessVersion = 1;

    float acceptThreshold = 0.35f;
    std::string thresholdVersionId = "thr_v1";
    int consecutivePassesToTrigger = 1;

    std::string galleryDir;
    std::size_t topK = 5;

    std::string faceSelectPolicy = "score_area";

    // INT8 量化模型开关。语义（原「无任何生产者」已不成立，2026-09 补齐）：
    //   · **谁该设**：只有「直接构造 FaceInferRequest 的调用方」——`rk3288_cli` 的 `--face-infer`
    //     与测试。⚠️ `--face-baseline` **也解析** `--int8 1|0` 并写入本字段，但基准路径自建
    //     det / emb、**不调用 runFaceInferOnce**（见 src/cpp/main.cpp 该子命令的解析处注释）
    //     ⇒ 该字段在基准里**零效果（0 行日志、不发生 INT8 尝试）**，把它与 `--face-infer` 并列
    //     为「有效生产者」是失真的（本批订正）。
    //   · **在什么条件下设**：仅当 `arcModelPath` / `yoloModelPath` 指向**已量化**的
    //     ncnn INT8 产物（`.param`/`.bin`）且 `ModelRegistry` 中相应条目已注册时才设为 true。
    //     本仓 INT8 条目是**按文件存在与否条件注册**的（`models/arcface_int8_ncnn/`、
    //     `models/mobilefacenet_int8_ncnn/`、`models/yolo_face_int8_ncnn/` 下的 `.param`），
    //     缺少模型文件（⇒ 该条目**未注册**）时置 true **无功能副作用**（回落 FP32），但
    //     **日志行数因消费点而异**，原「只会多一行 warn」的两种方向都不成立，如实订正如下：
    //       · 检测支 `detectFaces()`：`createDetector("yolo_face_int8")` 未传 err ⇒ 条目缺失时
    //         静默返回 nullptr；但该段末尾的 warn 在 `if (req.int8Enabled)` 内**无条件**执行
    //         ⇒ **1 行**（`load=` / `detect=` 均为空）。注意该行只在本回落段可达时出现，即
    //         上游 FP32 检测器注入也失败时（见该段 ⚠️ 可达性说明）。
    //       · 嵌入支 `computeEmbedding()`：`createEmbedder(eId)` 未传 err ⇒ 条目缺失时静默返回
    //         nullptr（ModelRegistry.cpp 的 embedder_not_found 分支因 `err == nullptr` **既不写
    //         err 也不记日志**），循环直接跳过 ⇒ **0 行**。
    //       · 若条目已注册但 load / embed 失败：检测支 1 行；嵌入支**每支一行**
    //         （`arcface_int8` + `mobilefacenet_int8` ⇒ 2 行）。故「无副作用」指**行为**，
    //         不等于「恒为 1 行日志」。
    //   · **消费点**：`FaceInferStages::detectFaces()` / `computeEmbedding()` 的回落分支（在
    //     `ctx.detector` / `ctx.embedder` 注入失败之后才可达）——不是「FP32 之外的优先路径」。
    //   · ⚠️ `WinConfig.model.int8Enabled` **不是**本字段的生产者：Windows 运行时（FramePipeline /
    //     RuntimeBootstrap）不构造 `FaceInferRequest`，该键只影响 RuntimeBootstrap 启动期的模型快照
    //     与告警（见 src/win/src/RuntimeBootstrap.cpp 的 INT8 消费段）。两者同名但不同通路。
    // 默认 false ⇒ 上述 INT8 分支不执行、零行为变化。
    bool int8Enabled = false;
    bool assumeL2Normalized = true;

    bool fakeDetect = false;
    bool fakeEmbedding = false;
};

struct FaceInferOutcome {
    bool ok = false;
    int errorCode = 0;
    std::string stage;
    std::string message;

    std::string json;
    std::string auditDir;
    std::string auditFilename;
};

FaceInferOutcome runFaceInferOnce(const FaceInferRequest& req);

} // namespace rk_core
