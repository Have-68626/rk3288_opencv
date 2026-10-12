#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// FaceSearchLinearIndex 是**全局类型**（声明于 FaceSearch.h，不在 rk_core 命名空间内）。
// 在 namespace 外前向声明，供 FaceInferRequest 以 shared_ptr<const ...> 持有不可变图库快照。
class FaceSearchLinearIndex;

namespace rk_core {

struct FaceInferRequest {
    std::string imagePath;

    // ── §I3 单次管线统一装配：区分「模型 ID」与「后端名」 ──────────────────────────
    // detectorId/embedderId 决定**装配哪个模型适配器**（经 ModelRegistry 按 ID 路由）：
    //   检测：yolo_face（默认，注册表 YoloFaceAdapter）/ scrfd_0.5gf（需 profile 提供解码语义）；
    //   嵌入：arcface（默认）/ mobilefacenet（显式开发用）。
    // 未知 ID ⇒ 装配期明确失败（不再按 yoloBackend/arcBackend 查注册表，消除隐式回落）。
    // 保留旧 yoloBackend/arcBackend 作为**后端名**（opencv/opencv_dnn/ncnn），仅决定所选适配器的后端。
    std::string detectorId = "yolo_face";
    std::string embedderId = "arcface";

    // 非空时：runFaceInferOnce 会先经 applyFaceProfileToRequest 读 profileDir/manifest.json，
    // 由 profile 一次性派生 detectorId/embedderId/双后端/模型绝对路径/节点/尺寸/阈值/int8Enabled，
    // 并通过 profile 为 SCRFD 提供解码语义。空 ⇒ 走旧的显式参数路径。
    std::string faceProfileDir;

    // 已解密并绑定的**不可变**内存图库快照（Android 侧经 replaceGalleryForSpace 建立）。
    // 非空时 loadGallery 直接绑定该快照、不读磁盘；searchTopK 直接检索该索引，不重复复制模板。
    std::shared_ptr<const ::FaceSearchLinearIndex> gallerySnapshot;

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

    // INT8 意图标志（§I3 起语义已变，旧「触发同路径 INT8 候选回落链」的行为已删除）。
    //   · **谁该设**：profile 模式由 `applyFaceProfileToRequest` 按 manifest 双 precision 派生
    //     （detector 与 embedder 均为 `int8` ⇒ true）；直接构造 FaceInferRequest 的调用方
    //     （`rk3288_cli --face-infer` 的 `--int8 1|0`、测试）也可显式设置。
    //   · **语义**：**不再是**「先试 INT8 再回落 FP32」的开关 —— 装配集中在 `runFaceInferOnce`，
    //     检测/嵌入适配器各由 `detectorId`/`embedderId` 唯一选定，**不做盲试式回落**。本字段只
    //     作为「期望精度」被装配缓存 key 与一致性判据（`int8Enabled=true` 只允许 ncnn 且量化资产）
    //     消费，不改变所选 ID。
    //   · ⚠️ `WinConfig.model.int8Enabled` **不是**本字段的生产者（Windows 运行时不构造
    //     FaceInferRequest，该键只影响 RuntimeBootstrap 启动期快照与告警）。两者同名但不同通路。
    // 默认 false。
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
