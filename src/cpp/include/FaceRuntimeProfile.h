#pragma once

#include <string>
#include <vector>

namespace rk_core {

// manifest v1 中模型文件条目：相对路径 + 期望 SHA-256（禁绝对路径/..）
struct FaceModelManifestEntry {
    std::string relativePath;  // 相对 manifest 基目录，禁绝对路径与父目录段
    std::string sha256;        // 64 位 hex；空表示不校验
};

// 模型运行时配置快照（任务9）：manifest v1 解析产物，供 RealtimeFacePipeline（任务11）装配。
struct FaceRuntimeProfile {
    int schemaVersion = 1;
    std::string profileId;
    std::string displayName;
    std::vector<std::string> supportedAbis;

    // detector（SCRFD ncnn：输入节点名 input.1、out0-8 输出；节点名随 manifest）
    // 缺键回落 = "input.1"——本仓 **`models/` 下全部 10 个 SCRFD `.param`**（含 int8 侧的 2 个）
    // 第 3 行皆为 `Input input.1`，与 manifest 用 sha256 钉住的 scrfd_500m/2.5g_kps-opt2.param 一致。
    // ⚠️ **勿把该结论推广到 `deps/insightface/det_10g.ncnn.param`**：那份**未经 PNNX 转换的 ncnn 版**
    //    （同为 SCRFD-10G 系）第 3 行实测是 `Input in0`（2026-09-26 二次审计实测）。
    // "in0" 是 ArcFace 侧名字（arcface.param 第 3 行），曾被误抄到检测器侧。
    FaceModelManifestEntry detectorParam;
    FaceModelManifestEntry detectorBin;
    std::string detectorInput = "input.1";
    std::vector<std::string> detectorOutputs;  // 9 个：score×3/bbox×3/kps×3，缺省走约定
    int detectorInputW = 640;
    int detectorInputH = 640;

    // detector 解码语义（P1-5，可选字段；缺省保持历史兼容）。
    // 真实 SCRFD（det_10g / 500m_kps / 2.5g_kps）为 distance 解码 + 每位置 2 anchor；
    // 输出布局视 .param 输出 blob 而定：原生 score_8/bbox_8/kps_8 为 NCHW，
    // PNNX 展平 out0..out8 为 flat，缺省 auto 由形状嗅探。
    enum class DetectorDecodeMode { kDelta = 0, kDistance = 1 };
    enum class DetectorOutputLayout { kAuto = 0, kNChw = 1, kFlat = 2 };
    DetectorDecodeMode detectorDecodeMode = DetectorDecodeMode::kDelta;
    int detectorAnchorsPerPos[3] = {1, 1, 1};
    DetectorOutputLayout detectorOutputLayout = DetectorOutputLayout::kAuto;

    // embedder（ArcFace：in0/out0、112×112、512 维）
    FaceModelManifestEntry embedderParam;
    FaceModelManifestEntry embedderBin;
    std::string embedderInput = "in0";
    std::string embedderOutput = "out0";
    int embedderInputW = 112;
    int embedderInputH = 112;
    int embeddingDim = 512;

    // 精度（§I2 独立图库契约）：仅 "fp32"/"int8"；缺省 "fp32" 保持旧 manifest 读取兼容。
    // 检测与识别精度必须一致——新 INT8 profile 要求两者皆为 int8（见 parseV1 的一致性判据）。
    std::string detectorPrecision = "fp32";
    std::string embedderPrecision = "fp32";

    // 独立图库空间 ID（§I2）：64 位**小写** hex；旧 manifest 缺省为空 ⇒ 继续走旧图库路径，
    // 不自动升级、不搬库、不导入旧模板。发行方按 SHA256(composeTemplateSpaceHashInput(四个
    // FP32 文件 sha256)) 计算：FP32 与其量化版**共用同一值**（INT8 文件哈希**不**参与，
    // 否则对照版本无法共享空间）。仅当其为已校验 hex 时才可拼入图库目录，禁止
    // displayName/profileId 或未校验路径直接拼入。
    std::string templateSpaceId;

    // 阈值（计划固定值；均为 (0,1] 合法）
    float detectionConfidence = 0.55f;
    float nmsIou = 0.40f;
    float identitySimilarity = 0.45f;

    std::string modelVersion;
    std::string licenseId;
};

class FaceRuntimeManifest {
public:
    // 解析 manifest v1 JSON → profile。非法（未知 schema/绝对路径/../ABI 不匹配/维度/节点/阈值非法）→ false + err。
    static bool parseV1(const std::string& jsonText, const std::string& currentAbi,
                        FaceRuntimeProfile& out, std::string& err);

    // 相对路径安全校验：禁绝对路径（/、\、盘符）与父目录段 `..`
    static bool isSafeRelativePath(const std::string& p);

    // 文件级校验（在 manifest 基目录解析相对路径）：存在、非 Git LFS pointer、SHA-256 匹配（若有）。
    static bool verifyFiles(const FaceRuntimeProfile& profile, const std::string& baseDir,
                            std::string& err);

    // 是否为 Git LFS pointer 文本（行首 "version https://git-lfs.github.com/spec/v1"）
    static bool fileIsLfsPointer(const std::string& path);

    // §I2 独立图库契约串（固定；改动会使所有既有 templateSpaceId 失效，故为常量）。
    static const char* templateSpaceContract();

    // §I2 空间 ID 的哈希输入：四个**父 FP32** 文件 sha256 按 det_param/det_bin/emb_param/emb_bin
    // 顺序以 '\n' 连接、再接契约串（同样以 '\n' 分隔），整体 UTF-8 编码。
    // 发行方对该串取 SHA256 的小写 hex 即得 templateSpaceId。
    // 注意：传入的应是 FP32 文件哈希；INT8 文件哈希**不**参与（FP32 与其量化版共用同一空间）。
    static std::string composeTemplateSpaceHashInput(const std::string& detParamSha,
                                                     const std::string& detBinSha,
                                                     const std::string& embParamSha,
                                                     const std::string& embBinSha);

    // templateSpaceId 合法性：恰 64 位**小写** hex（[0-9a-f]）。
    // 与 sha256 的大小写不敏感校验**不同**：空间 ID 会被直接拼进图库目录名，收紧为小写可避免
    // 同一逻辑空间出现大小写两份目录；拼接图库路径前必须过此校验。
    static bool isValidTemplateSpaceId(const std::string& s);
};

}  // namespace rk_core