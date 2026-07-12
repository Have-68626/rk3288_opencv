#pragma once

#include "FaceDetections.h"

#if __has_include(<opencv2/core.hpp>) && !defined(RK_SKIP_OPENCV)
#include <opencv2/core.hpp>
#else
namespace cv { class Mat; }
#endif

#include <string>

struct FaceAlignOptions {
    int outW = 112;
    int outH = 112;
    bool preferKeypoints5 = true;
};

struct FaceAlignResult {
    cv::Mat alignedBgr;
    bool usedKeypoints5 = false;
    bool usedBboxFallback = false;
    std::string err;
};

FaceAlignResult alignFaceForArcFace112(const cv::Mat& bgr, const FaceDetection& det, const FaceAlignOptions& opt);

