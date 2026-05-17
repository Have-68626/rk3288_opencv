#pragma once

#include "FaceDetections.h"
#include "FaceSearch.h"

#include <opencv2/core.hpp>

#include <string>
#include <vector>

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
};

