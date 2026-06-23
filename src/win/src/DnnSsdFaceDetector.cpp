#include "rk_win/DnnSsdFaceDetector.h"
#include "FileHash.h"

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>

namespace rk_win {

struct DnnSsdFaceDetector::Impl {
    DnnSsdConfig cfg{};
    cv::dnn::Net net;
    bool ready = false;
    bool streamWarm = false;

    static std::vector<DnnFaceDetection> parseSsdOutput(const cv::Mat& out, int imgW, int imgH, double confThr) {
        std::vector<DnnFaceDetection> dets;
        if (out.empty()) return dets;
        if (out.dims != 4) return dets;
        if (out.size[3] != 7) return dets;

        const int num = out.size[2];
        const float* data = reinterpret_cast<const float*>(out.data);
        dets.reserve(static_cast<size_t>(num));
        for (int i = 0; i < num; i++) {
            const float conf = data[i * 7 + 2];
            if (conf < static_cast<float>(confThr)) continue;
            float x1 = data[i * 7 + 3];
            float y1 = data[i * 7 + 4];
            float x2 = data[i * 7 + 5];
            float y2 = data[i * 7 + 6];
            int ix1 = static_cast<int>(x1 * imgW + 0.5f);
            int iy1 = static_cast<int>(y1 * imgH + 0.5f);
            int ix2 = static_cast<int>(x2 * imgW + 0.5f);
            int iy2 = static_cast<int>(y2 * imgH + 0.5f);
            ix1 = std::clamp(ix1, 0, imgW - 1);
            iy1 = std::clamp(iy1, 0, imgH - 1);
            ix2 = std::clamp(ix2, 0, imgW - 1);
            iy2 = std::clamp(iy2, 0, imgH - 1);
            if (ix2 <= ix1 || iy2 <= iy1) continue;
            DnnFaceDetection d;
            d.rect = cv::Rect(ix1, iy1, ix2 - ix1, iy2 - iy1);
            d.confidence = conf;
            dets.push_back(std::move(d));
        }
        std::sort(dets.begin(), dets.end(), [](const DnnFaceDetection& a, const DnnFaceDetection& b) { return a.confidence > b.confidence; });
        return dets;
    }
};

DnnSsdFaceDetector::DnnSsdFaceDetector() = default;

DnnSsdFaceDetector::~DnnSsdFaceDetector() {
    shutdown();
}

bool DnnSsdFaceDetector::initialize(const DnnSsdConfig& cfg, std::string& error) {
    shutdown();
    impl_ = new Impl();
    impl_->cfg = cfg;

    if (impl_->cfg.modelPath.empty()) {
        error = "dnn.model_path 为空";
        shutdown();
        return false;
    }

    std::string hash = rk_wcfr::calculateSHA256(impl_->cfg.modelPath);
    if (hash.empty()) {
        std::cerr << "[Self-Check] ERROR: Failed to read DNN model: " << impl_->cfg.modelPath << std::endl;
        std::cerr << "[Self-Check] Please download the model and place it in the correct directory." << std::endl;
    } else {
        std::cout << "[Self-Check] Loaded DNN model: " << impl_->cfg.modelPath << " | SHA256: " << hash << std::endl;
    }

    try {
        if (!impl_->cfg.configPath.empty()) impl_->net = cv::dnn::readNet(impl_->cfg.modelPath.string(), impl_->cfg.configPath.string());
        else impl_->net = cv::dnn::readNet(impl_->cfg.modelPath.string());
    } catch (const cv::Exception& e) {
        error = std::string("readNet 失败: ") + e.what();
        shutdown();
        return false;
    }

    try {
        impl_->net.setPreferableBackend(impl_->cfg.backend);
        impl_->net.setPreferableTarget(impl_->cfg.target);
    } catch (const cv::Exception&) {
    }

    impl_->ready = true;
    impl_->streamWarm = false;
    return true;
}

void DnnSsdFaceDetector::resetForStream() {
    if (!impl_) return;
    impl_->streamWarm = false;
}

void DnnSsdFaceDetector::shutdown() {
    if (!impl_) return;
    delete impl_;
    impl_ = nullptr;
}

bool DnnSsdFaceDetector::ready() const {
    return impl_ && impl_->ready;
}

std::vector<DnnFaceDetection> DnnSsdFaceDetector::detect(const cv::Mat& bgr, double& latencyMsOut) {
    latencyMsOut = 0.0;
    if (!impl_ || !impl_->ready) return {};
    if (bgr.empty()) return {};

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    const cv::Scalar mean(impl_->cfg.meanB, impl_->cfg.meanG, impl_->cfg.meanR);
    const cv::Size inSize(std::max(1, impl_->cfg.inputWidth), std::max(1, impl_->cfg.inputHeight));
    cv::Mat blob = cv::dnn::blobFromImage(bgr, impl_->cfg.scale, inSize, mean, impl_->cfg.swapRB, false);

    cv::Mat out;
    try {
        impl_->net.setInput(blob);
        out = impl_->net.forward();
    } catch (const cv::Exception&) {
        return {};
    }

    const auto t1 = clock::now();
    latencyMsOut = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count();

    return Impl::parseSsdOutput(out, bgr.cols, bgr.rows, impl_->cfg.confThreshold);
}

}  // namespace rk_win

