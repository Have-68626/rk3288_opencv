#pragma once

#include "FaceDetections.h"

#if __has_include(<opencv2/core.hpp>) && !defined(RK_SKIP_OPENCV)
#include <opencv2/core.hpp>
#else
namespace cv { class Mat; }
#endif

#include <memory>
#include <string>

namespace rk_core {

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

    /**
     * @brief Checks whether a given backend name is supported by this implementation.
     * @param name Backend name (e.g. "opencv_dnn", "ncnn").
     * @return true if supported.
     */
    virtual bool supportsBackend(const char* name) const = 0;

    /**
     * @brief Switches to a different backend at runtime.
     * @param name Target backend name.
     * @param err Output error message on failure.
     * @return true if switch succeeded.
     */
    virtual bool switchBackend(const char* name, std::string& err) = 0;
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

} // namespace rk_core

