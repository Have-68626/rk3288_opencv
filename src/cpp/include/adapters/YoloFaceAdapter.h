#pragma once

#include "FaceDetector.h"
#include "YoloFaceDetector.h"

#include <mutex>
#include <string>

namespace rk_core {

class YoloFaceAdapter : public FaceDetector {
public:
    YoloFaceAdapter();
    ~YoloFaceAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    FaceDetections detect(const cv::Mat& bgr, std::string& err) override;
    const char* name() const override;

    // §I3 显式配置入口：调用后 load() 必须使用指定的后端/spec，
    // 不再按扩展名猜测后端（消除「传错 backend 仍按后缀静默换后端」的隐式行为）。
    // 未调用时保留历史「按扩展名选择后端」的兼容行为；configure 只记录配置，load 时才真正装载。
    void configureOpenCv(const YoloFaceModelSpec& spec, const YoloFaceOptions& opt);
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    void configureNcnn(const NcnnYoloFaceModelSpec& spec, const YoloFaceOptions& opt);
#endif

private:
    enum class ConfiguredBackend { kNone, kOpenCv, kNcnn };

    mutable std::mutex mu_;
    std::unique_ptr<YoloFaceDetector> inner_;
    std::string currentName_;
    YoloFaceOptions opt_;
    bool loaded_ = false;
    // 配置态：kNone ⇒ 历史按扩展名选择；其余 ⇒ load 强制使用对应后端与 spec。
    ConfiguredBackend configuredBackend_ = ConfiguredBackend::kNone;
    YoloFaceModelSpec ocvSpec_;
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    NcnnYoloFaceModelSpec ncnnSpec_;
#endif
};

} // namespace rk_core
