#pragma once

#include "Embedder.h"
#include "FaceDetections.h"
#include "FaceSearch.h"

#include <opencv2/core.hpp>

#include <memory>
#include <string>
#include <vector>

namespace rk_core {

// Forward declaration for externally-injected instance (生命期由调用方管理)
class FaceDetector;

struct FaceInferMetrics {
    long long msLoad = 0;
    long long msDetect = 0;
    long long msAlign = 0;
    long long msEmbed = 0;
    long long msSearch = 0;
    long long msTotal = 0;
};

struct FaceInferContext {
    cv::Mat img;

    FaceDetections faces;
    bool hasFace = false;
    FaceDetection mainFace;

    cv::Mat aligned112;
    bool usedKeypoints5 = false;
    bool usedBboxFallback = false;

    std::vector<float> embedding;
    bool embeddingOk = false;
    bool embeddingFake = false;

    std::string yoloBackendName;
    std::string arcBackendName;

    std::vector<FaceSearchEntry> galleryEntries;
    std::vector<std::string> galleryWarnings;

    // §I3：不可变内存图库快照（来自 req.gallerySnapshot）。非空时 loadGallery 直接绑定、
    // searchTopK 直接检索该索引，不再从 galleryEntries 重建索引。
    std::shared_ptr<const ::FaceSearchLinearIndex> gallerySnapshot;

    FaceSearchLinearIndex index;
    std::vector<FaceSearchHit> hits;
    float bestScore = -1.0f;
    bool hasCandidate = false;

    // 外部注入的推理实例指针（由调用方管理生命周期，可为 nullptr）
    // §I3：装配集中在 runFaceInferOnce——detectFaces/computeEmbedding 只消费注入实例，
    // 不再自行创建/回落；装配失败时经下面的 *LoadError 带出真实原因。
    // detector 为 FaceDetector*，embedder 改为统一接口 rk_core::Embedder*（非 ArcFaceEmbedder*），
    // 阶段调用 embed() 取 vector，不再假定 ArcFaceEmbedding 结构。
    FaceDetector* detector = nullptr;
    Embedder* embedder = nullptr;

    // 装配期真实错误（正常图像加载失败仍优先 image_load；空图/无脸时不凭模型状态制造成功）。
    std::string detectorLoadError;
    std::string embedderLoadError;
};

} // namespace rk_core

