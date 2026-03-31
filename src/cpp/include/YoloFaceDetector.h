#pragma once

#include "FaceDetections.h"

#include <opencv2/core.hpp>

#include <memory>
#include <string>

struct YoloFaceOptions {
    int inputW = 320;
    int inputH = 320;
    float scoreThreshold = 0.40f;
    float nmsIouThreshold = 0.45f;
    bool enableKeypoints5 = true;

    bool letterbox = true;
    float scale = 1.0f / 255.0f;
    bool swapRB = true;
    int meanB = 0;
    int meanG = 0;
    int meanR = 0;

    int opencvBackend = 0;
    int opencvTarget = 0;
};

struct YoloFaceModelSpec {
    std::string modelPath;
    std::string configPath;
    std::string framework;
    std::string outputName;
};

class YoloFaceDetector {
public:
    virtual ~YoloFaceDetector() = default;
    virtual bool load(const YoloFaceModelSpec& spec, const YoloFaceOptions& opt, std::string& err) = 0;
    virtual FaceDetections detect(const cv::Mat& bgr, std::string& err) = 0;
    virtual const char* backendName() const = 0;
};

std::unique_ptr<YoloFaceDetector> CreateOpenCvDnnYoloFaceDetector();

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
struct NcnnYoloFaceModelSpec {
    std::string paramPath;
    std::string binPath;
    std::string inputName = "data";
    std::string outputName = "output";
    int threads = 1;
    bool lightmode = true;
};

std::unique_ptr<YoloFaceDetector> CreateNcnnYoloFaceDetector(const NcnnYoloFaceModelSpec& ncnnSpec);
#endif

