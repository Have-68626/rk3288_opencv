#include "ArcFaceEmbedder.h"

#include "NativeLog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
#include <net.h>
#endif

namespace {

// Optimized to use float instead of double to maximize SIMD vectorization
// and improve execution speed on the RK3288 (ARM Cortex-A17) platform.
static bool l2NormalizeInplace(std::vector<float>& v) {
    if (v.empty()) return false;
    float s = 0.0f;
    for (float x : v) s += x * x;
    const float n = std::sqrt(s);
    if (!(n > 0.0f)) return false;
    const float inv = 1.0f / n;
    for (float& x : v) x *= inv;
    return true;
}

static std::vector<float> takeFirst512AsF32(const cv::Mat& m) {
    if (m.empty()) return {};
    cv::Mat f;
    if (m.depth() == CV_32F) {
        f = m;
    } else {
        m.convertTo(f, CV_32F);
    }

    if (!f.isContinuous()) f = f.clone();
    const std::size_t total = static_cast<std::size_t>(f.total());
    if (total < static_cast<std::size_t>(ArcFaceEmbedding::kDim)) return {};
    std::vector<float> out(static_cast<std::size_t>(ArcFaceEmbedding::kDim));
    std::memcpy(out.data(), f.ptr<float>(), out.size() * sizeof(float));
    return out;
}

}  // namespace

struct ArcFaceEmbedder::OpenCvDnnState {
    cv::dnn::Net net;
};

struct ArcFaceEmbedder::NcnnState {
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    ncnn::Net net;
#endif
};

bool ArcFaceEmbedder::initialize(const ArcFaceEmbedderConfig& cfg, std::string* err) {
    cfg_ = cfg;
    inited_ = false;
    ocv_.reset();
    ncnn_.reset();

    if (cfg_.inputW <= 0 || cfg_.inputH <= 0) {
        if (err) *err = "ArcFaceEmbedder: 输入尺寸非法";
        return false;
    }

    if (cfg_.backend == ArcFaceEmbedderConfig::BackendType::OpenCvDnn) {
        if (cfg_.opencvModel.empty()) {
            if (err) *err = "ArcFaceEmbedder: 缺少 OpenCV DNN 模型路径";
            return false;
        }

        auto st = std::make_shared<OpenCvDnnState>();
        try {
            if (!cfg_.opencvConfig.empty() && !cfg_.opencvFramework.empty()) {
                st->net = cv::dnn::readNet(cfg_.opencvModel, cfg_.opencvConfig, cfg_.opencvFramework);
            } else if (!cfg_.opencvConfig.empty()) {
                st->net = cv::dnn::readNet(cfg_.opencvModel, cfg_.opencvConfig);
            } else {
                st->net = cv::dnn::readNet(cfg_.opencvModel);
            }
        } catch (const cv::Exception& e) {
            if (err) *err = std::string("ArcFaceEmbedder: OpenCV readNet 失败: ") + e.what();
            return false;
        }

        try {
            st->net.setPreferableBackend(cfg_.opencvBackend);
            st->net.setPreferableTarget(cfg_.opencvTarget);
        } catch (const cv::Exception&) {
        }

        ocv_ = std::move(st);
        inited_ = true;
        return true;
    } else if (cfg_.backend == ArcFaceEmbedderConfig::BackendType::Qualcomm) {
        // [Qualcomm SDK Placeholder]
        // 探测失败或硬件不兼容时回退到 CPU (OpenCV DNN)
        rklog::logInfo("ArcFaceEmbedder", "initQualcommDelegate", "Qualcomm SDK fallback to CPU... 待补测");
        if (cfg_.opencvModel.empty()) {
            if (err) *err = "ArcFaceEmbedder: 缺少回退的 OpenCV DNN 模型路径";
            return false;
        }

        auto st = std::make_shared<OpenCvDnnState>();
        try {
            if (!cfg_.opencvConfig.empty() && !cfg_.opencvFramework.empty()) {
                st->net = cv::dnn::readNet(cfg_.opencvModel, cfg_.opencvConfig, cfg_.opencvFramework);
            } else if (!cfg_.opencvConfig.empty()) {
                st->net = cv::dnn::readNet(cfg_.opencvModel, cfg_.opencvConfig);
            } else {
                st->net = cv::dnn::readNet(cfg_.opencvModel);
            }
        } catch (const cv::Exception& e) {
            if (err) *err = std::string("ArcFaceEmbedder: OpenCV readNet 失败: ") + e.what();
            return false;
        }

        try {
            st->net.setPreferableBackend(cfg_.opencvBackend);
            st->net.setPreferableTarget(cfg_.opencvTarget);
        } catch (const cv::Exception&) {
        }

        ocv_ = std::move(st);
        inited_ = true;
        return true;
    }

    if (cfg_.backend == ArcFaceEmbedderConfig::BackendType::Ncnn) {
#if !(defined(RK_HAVE_NCNN) && RK_HAVE_NCNN)
        if (err) *err = "ArcFaceEmbedder: 当前构建未启用 ncnn";
        return false;
#else
        if (cfg_.ncnnParam.empty() || cfg_.ncnnBin.empty()) {
            if (err) *err = "ArcFaceEmbedder: 缺少 ncnn param/bin 路径";
            return false;
        }

        auto st = std::make_shared<NcnnState>();
        st->net.opt.num_threads = std::max(1, cfg_.ncnnThreads);
        st->net.opt.lightmode = cfg_.ncnnLightmode;

        if (st->net.load_param(cfg_.ncnnParam.c_str()) != 0) {
            if (err) *err = "ArcFaceEmbedder: ncnn load_param 失败";
            return false;
        }
        if (st->net.load_model(cfg_.ncnnBin.c_str()) != 0) {
            if (err) *err = "ArcFaceEmbedder: ncnn load_model 失败";
            return false;
        }

        ncnn_ = std::move(st);
        inited_ = true;
        return true;
#endif
    }

    if (err) *err = "ArcFaceEmbedder: 未知后端";
    return false;
}

bool ArcFaceEmbedder::isInitialized() const {
    return inited_;
}

std::optional<ArcFaceEmbedding> ArcFaceEmbedder::embedAlignedFaceBgr(const cv::Mat& alignedFaceBgr, std::string* err) const {
    if (!inited_) {
        if (err) *err = "ArcFaceEmbedder: 未初始化";
        return std::nullopt;
    }
    if (alignedFaceBgr.empty()) {
        if (err) *err = "ArcFaceEmbedder: 输入图像为空";
        return std::nullopt;
    }

    if (cfg_.backend == ArcFaceEmbedderConfig::BackendType::OpenCvDnn || cfg_.backend == ArcFaceEmbedderConfig::BackendType::Qualcomm) {
        if (!ocv_) {
            if (err) *err = "ArcFaceEmbedder: OpenCV DNN 状态缺失 (Fallback failed)";
            return std::nullopt;
        }

        cv::Mat resized;
        cv::resize(alignedFaceBgr, resized, cv::Size(cfg_.inputW, cfg_.inputH), 0, 0, cv::INTER_LINEAR);

        const cv::Scalar mean(cfg_.meanB, cfg_.meanG, cfg_.meanR);
        cv::Mat blob = cv::dnn::blobFromImage(resized, cfg_.scale, cv::Size(cfg_.inputW, cfg_.inputH), mean, cfg_.swapRB, false);

        cv::Mat out;
        try {
            if (!cfg_.opencvInput.empty()) ocv_->net.setInput(blob, cfg_.opencvInput);
            else ocv_->net.setInput(blob);

            if (cfg_.opencvOutput.empty()) out = ocv_->net.forward();
            else out = ocv_->net.forward(cfg_.opencvOutput);
        } catch (const cv::Exception& e) {
            if (err) *err = std::string("ArcFaceEmbedder: OpenCV forward 失败: ") + e.what();
            return std::nullopt;
        }

        ArcFaceEmbedding emb;
        emb.modelVersion = cfg_.modelVersion;
        emb.preprocessVersion = cfg_.preprocessVersion;
        emb.values = takeFirst512AsF32(out);
        if (emb.values.size() != static_cast<std::size_t>(ArcFaceEmbedding::kDim)) {
            if (err) *err = "ArcFaceEmbedder: 输出维度不满足 512";
            return std::nullopt;
        }
        if (!l2NormalizeInplace(emb.values)) {
            if (err) *err = "ArcFaceEmbedder: L2 归一化失败";
            return std::nullopt;
        }
        return emb;
    }

    if (cfg_.backend == ArcFaceEmbedderConfig::BackendType::Ncnn) {
#if !(defined(RK_HAVE_NCNN) && RK_HAVE_NCNN)
        if (err) *err = "ArcFaceEmbedder: 当前构建未启用 ncnn";
        return std::nullopt;
#else
        if (!ncnn_) {
            if (err) *err = "ArcFaceEmbedder: ncnn 状态缺失";
            return std::nullopt;
        }

        cv::Mat resized;
        cv::resize(alignedFaceBgr, resized, cv::Size(cfg_.inputW, cfg_.inputH), 0, 0, cv::INTER_LINEAR);

        cv::Mat src = resized;
        if (cfg_.swapRB) {
            cv::Mat rgb;
            cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
            src = std::move(rgb);
        }

        const int w = cfg_.inputW;
        const int h = cfg_.inputH;
        ncnn::Mat in = ncnn::Mat::from_pixels(src.data, cfg_.swapRB ? ncnn::Mat::PIXEL_RGB : ncnn::Mat::PIXEL_BGR, w, h);

        float meanVals[3];
        if (cfg_.swapRB) {
            meanVals[0] = cfg_.meanR;
            meanVals[1] = cfg_.meanG;
            meanVals[2] = cfg_.meanB;
        } else {
            meanVals[0] = cfg_.meanB;
            meanVals[1] = cfg_.meanG;
            meanVals[2] = cfg_.meanR;
        }
        float normVals[3] = {cfg_.scale, cfg_.scale, cfg_.scale};
        in.substract_mean_normalize(meanVals, normVals);

        ncnn::Extractor ex = ncnn_->net.create_extractor();
        ex.set_num_threads(std::max(1, cfg_.ncnnThreads));
        if (ex.input(cfg_.ncnnInput.c_str(), in) != 0) {
            if (err) *err = "ArcFaceEmbedder: ncnn input 失败";
            return std::nullopt;
        }

        ncnn::Mat out;
        if (ex.extract(cfg_.ncnnOutput.c_str(), out) != 0) {
            if (err) *err = "ArcFaceEmbedder: ncnn extract 失败";
            return std::nullopt;
        }

        const std::size_t total = static_cast<std::size_t>(out.total());
        if (total < static_cast<std::size_t>(ArcFaceEmbedding::kDim)) {
            if (err) *err = "ArcFaceEmbedder: ncnn 输出维度不满足 512";
            return std::nullopt;
        }

        ArcFaceEmbedding emb;
        emb.modelVersion = cfg_.modelVersion;
        emb.preprocessVersion = cfg_.preprocessVersion;
        emb.values.assign(out.begin(), out.begin() + ArcFaceEmbedding::kDim);
        if (!l2NormalizeInplace(emb.values)) {
            if (err) *err = "ArcFaceEmbedder: L2 归一化失败";
            return std::nullopt;
        }
        return emb;
#endif
    }

    if (err) *err = "ArcFaceEmbedder: 未知后端";
    return std::nullopt;
}

