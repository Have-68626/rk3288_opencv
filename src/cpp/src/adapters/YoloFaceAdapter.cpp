#include "YoloFaceAdapter.h"

#include "NativeLog.h"

using namespace rk_core;

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

void YoloFaceAdapter::configureOpenCv(const YoloFaceModelSpec& spec, const YoloFaceOptions& opt) {
    configuredBackend_ = ConfiguredBackend::kOpenCv;
    ocvSpec_ = spec;
    opt_ = opt;
}

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
void YoloFaceAdapter::configureNcnn(const NcnnYoloFaceModelSpec& spec, const YoloFaceOptions& opt) {
    configuredBackend_ = ConfiguredBackend::kNcnn;
    ncnnSpec_ = spec;
    opt_ = opt;
}
#endif

bool YoloFaceAdapter::load(const std::string& modelPath, std::string& err) {
    loaded_ = false;
    inner_.reset();

    // §I3：显式配置后强制使用指定后端/spec，不再按扩展名覆盖。
    if (configuredBackend_ == ConfiguredBackend::kOpenCv) {
        YoloFaceModelSpec spec = ocvSpec_;
        if (spec.modelPath.empty()) spec.modelPath = modelPath;
        inner_ = CreateOpenCvDnnYoloFaceDetector();
        if (!inner_) {
            err = "opencv_detector_factory_failed";
            return false;
        }
        if (!inner_->load(spec, opt_, err)) {
            // 不回落其它后端：后端由装配期显式决定，错配应明确失败。
            return false;
        }
        currentName_ = "opencv_dnn";
        loaded_ = true;
        return true;
    }
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    if (configuredBackend_ == ConfiguredBackend::kNcnn) {
        inner_ = CreateNcnnYoloFaceDetector(ncnnSpec_);
        if (!inner_) {
            err = "ncnn_detector_factory_failed";
            return false;
        }
        YoloFaceModelSpec dummy;
        if (!inner_->load(dummy, opt_, err)) {
            rklog::logWarn("YoloFaceAdapter", "load", "ncnn failed: " + err);
            return false;
        }
        currentName_ = "ncnn";
        loaded_ = true;
        return true;
    }
#endif

    // 未配置：历史兼容——按扩展名选择后端。
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
            rklog::logWarn("YoloFaceAdapter", "load",
                "ncnn failed: " + err + "; .param/.bin cannot be used with OpenCV DNN");
            return false;
        }
        currentName_ = "ncnn";
#else
        rklog::logWarn("YoloFaceAdapter", "load",
            "ncnn not available for .param/.bin, cannot load model: " + modelPath);
        err = "ncnn unavailable for .param/.bin model";
        return false;
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
