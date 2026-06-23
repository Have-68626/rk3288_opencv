#include "YoloFaceAdapter.h"

#include "NativeLog.h"

#include <filesystem>

static constexpr int kDefaultInputW = 320;
static constexpr int kDefaultInputH = 320;

YoloFaceAdapter::YoloFaceAdapter() {
    opt_.inputW = kDefaultInputW;
    opt_.inputH = kDefaultInputH;
    opt_.scoreThreshold = 0.40f;
    opt_.nmsIouThreshold = 0.45f;
    opt_.enableKeypoints5 = true;
    opt_.letterbox = true;
    opt_.scale = 1.0f / 255.0f;
    opt_.swapRB = true;
}

YoloFaceAdapter::~YoloFaceAdapter() = default;

bool YoloFaceAdapter::load(const std::string& modelPath, std::string& err) {
    loaded_ = false;
    inner_.reset();

    std::string ext = std::filesystem::path(modelPath).extension().string();

    if (ext == ".param" || ext == ".bin") {
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
        std::string paramPath = modelPath;
        std::string binPath;
        if (ext == ".param") {
            binPath = std::filesystem::path(modelPath).replace_extension(".bin").string();
        } else {
            binPath = modelPath;
            paramPath = std::filesystem::path(modelPath).replace_extension(".param").string();
        }

        NcnnYoloFaceModelSpec ns;
        ns.paramPath = paramPath;
        ns.binPath = binPath;
        ns.inputName = "data";
        ns.outputName = "output";
        inner_ = CreateNcnnYoloFaceDetector(ns);

        YoloFaceModelSpec dummy;
        if (!inner_->load(dummy, opt_, err)) {
            rklog::logWarn("YoloFaceAdapter", "load", "ncnn failed: " + err + ", falling back to OpenCV DNN");
            inner_ = CreateOpenCvDnnYoloFaceDetector();
            YoloFaceModelSpec spec;
            spec.modelPath = modelPath;
            if (!inner_->load(spec, opt_, err)) {
                return false;
            }
            currentName_ = "opencv_dnn";
            loaded_ = true;
            return true;
        }
        currentName_ = "ncnn";
#else
        rklog::logWarn("YoloFaceAdapter", "load", "ncnn not available for .param/.bin, falling back to OpenCV DNN");
        inner_ = CreateOpenCvDnnYoloFaceDetector();
        YoloFaceModelSpec spec;
        spec.modelPath = modelPath;
        if (!inner_->load(spec, opt_, err)) {
            return false;
        }
        currentName_ = "opencv_dnn";
#endif
    } else {
        inner_ = CreateOpenCvDnnYoloFaceDetector();
        YoloFaceModelSpec spec;
        spec.modelPath = modelPath;
        if (!inner_->load(spec, opt_, err)) {
            return false;
        }
        currentName_ = "opencv_dnn";
    }

    loaded_ = true;
    return true;
}

FaceDetections YoloFaceAdapter::detect(const cv::Mat& bgr, std::string& err) {
    if (!loaded_ || !inner_) {
        err = "detector_not_loaded";
        return {};
    }
    return inner_->detect(bgr, err);
}

const char* YoloFaceAdapter::name() const {
    return currentName_.c_str();
}
