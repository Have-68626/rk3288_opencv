#pragma once

#include "Compat.h"
#include <string>
#include <vector>

#ifndef RK_WIN_HAS_OPENCV
#if __has_include(<opencv2/core.hpp>)
#define RK_WIN_HAS_OPENCV 1
#else
#define RK_WIN_HAS_OPENCV 0
#endif
#endif

#if RK_WIN_HAS_OPENCV
#include <opencv2/core.hpp>
#else
namespace cv {
struct Mat;
#ifndef RK_WIN_STUB_CV_RECT_DEFINED
#define RK_WIN_STUB_CV_RECT_DEFINED 1
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};
#endif
}
#endif

namespace rk_win {

struct DnnFaceDetection {
    cv::Rect rect;
    float confidence = 0.0f;
};

struct DnnSsdConfig {
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

class DnnSsdFaceDetector {
public:
    DnnSsdFaceDetector();
    ~DnnSsdFaceDetector();

    bool initialize(const DnnSsdConfig& cfg, std::string& error);
    void resetForStream();
    void shutdown();

    bool ready() const;
    std::vector<DnnFaceDetection> detect(const cv::Mat& bgr, double& latencyMsOut);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace rk_win

