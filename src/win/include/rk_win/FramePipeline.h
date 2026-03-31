#pragma once

#include "FaceRecognizer.h"
#include "MfCamera.h"
#include "StructuredLogger.h"
#include "WinConfig.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#if __has_include(<optional>)
#include <optional>
#endif
#if !defined(__cpp_lib_optional)
namespace std {
template <class T>
class optional;
}
#endif
#include <string>
#include <thread>
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
struct Mat {
    int rows = 0;
    int cols = 0;
    void* data = nullptr;
};
}
#endif

namespace rk_win {

class DnnSsdFaceDetector;
class EventLogger;

struct RenderState {
    cv::Mat bgr;
    std::vector<FaceMatch> faces;
    double fps = 0.0;
    double inferMs = 0.0;
    double dropRate = 0.0;
    int stride = 1;
    std::uint64_t timestamp100ns = 0;
    std::string status;
};

struct CameraSwitchResult {
    bool ok = false;
    bool rolledBack = false;
    int width = 0;
    int height = 0;
    int fps = 0;
    std::int64_t totalMs = 0;
    std::int64_t cameraMs = 0;
    std::int64_t detectorMs = 0;
    std::string reason;
};

struct FacesSnapshot {
    std::vector<FaceMatch> faces;
    int frameWidth = 0;
    int frameHeight = 0;
    std::uint64_t timestamp100ns = 0;
    double inferMs = 0.0;
    double dropRate = 0.0;
    int stride = 1;
    int previewWidth = 0;
    int previewHeight = 0;
    int previewScaleMode = 0;
};

class FramePipeline {
public:
    FramePipeline();
    ~FramePipeline();

    bool initialize(const AppConfig& cfg);
    void shutdown();

    std::vector<CameraDevice> devices() const;
    std::vector<CameraFormat> formatsForDeviceIndex(int index) const;

    bool startCameraByIndex(int deviceIndex, int formatIndex);
    CameraSwitchResult applyCameraSettings(int deviceIndex, int width, int height, int fps, int maxTotalMs = 300);
    void requestStopCamera();

    void setFlip(bool flipX, bool flipY);
    void setEventLogger(EventLogger* logger);
    void setPreviewLayout(int previewW, int previewH, int previewScaleMode);

    void requestEnroll(const std::string& personId);
    void requestClearDb();

    bool tryGetRenderState(RenderState& out);
    bool snapshotFaces(FacesSnapshot& out);
    std::uint64_t currentFacesSeq() const;
    bool waitFacesSeqChanged(std::uint64_t lastSeq, int timeoutMs, std::uint64_t& outSeq) const;

    bool lastErrorIsPrivacyDenied() const;
    void openPrivacySettings() const;

private:
    void captureLoop();
    void processLoop();
    void stopCameraLocked();
    CameraSwitchResult startCameraWithRollbackLocked(int deviceIndex, const CameraFormat& desired, int maxTotalMs);
    std::optional<int> findExactFormatIndexLocked(int deviceIndex, int width, int height, int fps) const;

    AppConfig cfg_{};
    mutable std::mutex mu_;

    std::vector<CameraDevice> devices_;
    int activeDeviceIndex_ = -1;
    int activeFormatIndex_ = -1;
    CameraFormat desiredFormat_{};
    std::string activeCameraNameUtf8_;
    std::string activeCameraIdUtf8_;

    std::atomic<bool> running_{false};
    std::atomic<bool> cameraRunning_{false};
    std::thread captureThread_;
    std::thread processThread_;

    MfCamera camera_;
    StructuredLogger logger_;
    FaceRecognizer recognizer_;
    std::unique_ptr<DnnSsdFaceDetector> dnn_;
    std::mutex dnnMu_;

    bool flipX_ = false;
    bool flipY_ = false;

    std::mutex eventsMu_;
    EventLogger* events_ = nullptr;

    int detectStride_ = 1;

    std::mutex frameMu_;
    std::condition_variable frameCv_;
    bool hasFrame_ = false;
    cv::Mat latestFrame_;
    std::uint64_t latestFrameTs_ = 0;
    std::atomic<std::uint64_t> captureFrameCount_{0};
    std::atomic<std::uint64_t> overwriteDropCount_{0};

    std::mutex cameraSignalMu_;
    std::condition_variable cameraSignalCv_;
    int cameraSignal_ = 0;
    CameraOpenResult lastCameraOpenResult_{};

    std::mutex detectorSignalMu_;
    std::condition_variable detectorSignalCv_;
    int detectorSignal_ = 0;

    mutable std::mutex previewMu_;
    int previewW_ = 0;
    int previewH_ = 0;
    int previewScaleMode_ = 0;

    mutable std::mutex facesSeqMu_;
    mutable std::condition_variable facesSeqCv_;
    std::uint64_t facesSeq_ = 0;

    std::mutex renderMu_;
    RenderState render_{};
    std::uint64_t frameIndex_ = 0;

    std::atomic<bool> enrollRequested_{false};
    std::string enrollPersonId_;
    int enrollRemaining_ = 0;

    std::atomic<bool> clearDbRequested_{false};

    std::atomic<bool> lastPrivacyDenied_{false};
};

}  // namespace rk_win

