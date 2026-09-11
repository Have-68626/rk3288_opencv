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

    // detector（SCRFD ncnn 约定：in0 输入、out0-8 输出；节点名随 manifest）
    FaceModelManifestEntry detectorParam;
    FaceModelManifestEntry detectorBin;
    std::string detectorInput = "in0";
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
};

}  // namespace rk_core