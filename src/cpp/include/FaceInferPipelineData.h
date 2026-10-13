#pragma once

#include "FaceDetections.h"
#include "FaceSearch.h"

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace rk_core {

// Forward declarations for externally-injected instances (生命期由调用方管理)
class FaceDetector;
class ArcFaceEmbedder;

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

    FaceSearchLinearIndex index;
    std::vector<FaceSearchHit> hits;
    float bestScore = -1.0f;
    bool hasCandidate = false;

    // 外部注入的推理实例指针（由调用方管理生命周期，可为 nullptr）
    // 设置后 detectFaces/computeEmbedding 将优先使用注入实例而非自行创建
    FaceDetector* detector = nullptr;
    ArcFaceEmbedder* embedder = nullptr;
};

} // namespace rk_core

