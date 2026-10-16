/**
 * @file VideoManager.h
 * @brief Manages video capture device.
 * 
 * Handles camera initialization, frame grabbing, and resolution settings.
 * Implements a threaded capture mechanism to ensure low latency by always 
 * providing the most recent frame.
 */
#pragma once

#include <opencv2/opencv.hpp>
#include <opencv2/core/ocl.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class MppDecoder;

namespace rk_core {

class VideoManager {
public:
    VideoManager();
    ~VideoManager();

    void setCancelToken(std::atomic<bool>* token);
    void setTimeoutsMs(int openTimeoutMs, int readTimeoutMs);

    /**
     * @brief Set whether to use OpenCL globally.
     * @param requested true to request OpenCL enablement, false to disable.
     */
    void setUseOpenCL(bool requested);

    void setUseMpp(bool requested) { mppRequested_.store(requested); }
    bool mppRequested() const { return mppRequested_.load(); }
    bool mppEffective() const { return mppEffective_.load(); }
    std::uint64_t mppDecodedFrames() const { return mppDecodedFrames_.load(); }

    /**
     * @brief Opens the camera device.
     * @param deviceId Camera index (default 0).
     * @return true if successful.
     */
    bool open(int deviceId = 0);

    /**
     * @brief Opens a video file or image as a mock source.
     * @param filePath Path to file.
     * @return true if successful.
     */
    bool open(const std::string& filePath);

    /**
     * @brief Checks if the given path is a network stream URL.
     * @param path The path to check.
     * @return true if the path is a supported network URL (e.g., http, rtsp).
     */
    static bool isUrlSource(const std::string& path);

    /**
     * @brief Sanitizes a URI for log output by masking the userinfo segment.
     *
     * URI userinfo（scheme://user:pass@host）属敏感凭据：operator 输入的 RTSP/HTTP 流地址
     * 若含 userinfo，原样写入日志即端到端凭据泄漏（S-SEC-B07-002）。本函数仅作用于日志层：
     * 将 userinfo 段整体替换为 ***@（不暴露 user 也不暴露 pass），保留 scheme://host:port
     * 可读性；非 URL（无 scheme://，如本地文件路径）原样返回，不影响网络行为。
     * 纯函数，可独立单测（face_infer_unit_tests 的可测 seam）。
     *
     * @param url 待脱敏的 URL 或本地路径。
     * @return 已掩码 userinfo 的 URL；非 URL 输入原样返回。
     */
    static std::string sanitizeUriForLog(const std::string& url);

    /**
     * @brief Calling-phase preflight for mock input. Returns false with a reason code on fast reject.
     */
    static bool preflightMockInput(const std::string& filePath, std::string* reasonCode);

    /**
     * @brief Closes the camera device.
     */
    void close();

    /**
     * @brief Retrieves the latest captured frame.
     * @param outFrame Matrix to store the frame.
     * @return true if a valid frame is returned.
     */
    bool getLatestFrame(cv::Mat& outFrame);

    /**
     * @brief Checks if the camera is currently capturing.
     */
    bool isOpened() const;

    // ---- Mock Mode State ----

    /**
     * @brief Mock file loading state machine.
     */
    enum class MockState {
        NONE,             // Not in mock mode
        INIT,             // Mock mode initialized
        PREFLIGHT_OK,     // Preflight check passed
        LOADING,          // File/video loading in progress
        RUNNING,          // Mock source running normally
        FAILED,           // Mock source failed (corrupted/unsupported)
        FALLBACK          // Mock failed, fell back to camera
    };

    /**
     * @brief Returns the current mock state.
     */
    MockState getMockState() const;

    /**
     * @brief Returns the mock file path, empty if not in mock mode.
     */
    std::string getMockFilePath() const;

    /**
     * @brief Returns last mock reject/failure reason code for auditing.
     */
    std::string getLastMockRejectReason() const;

private:
    friend struct VideoManagerTestPeer;
    void setMockRejectReason(const std::string& reasonCode, const std::string& detail = "");
    void clearMockRejectReason();
    void captureLoop();

    cv::VideoCapture cap;
    std::thread captureThread;
    std::atomic<bool> isRunning;
    std::atomic<bool>* cancelToken = nullptr;
    int openTimeoutMs = 5000;
    int readTimeoutMs = 5000;
    
    cv::Mat latestFrame;
    std::mutex frameMutex;
    bool hasNewFrame;
    bool openCLRequested = false;
    
    // Mock Mode Support
    bool isMockMode = false;
    bool isStaticImage = false;
    cv::Mat staticFrame;
    std::unique_ptr<MppDecoder> mppDecoder;
    bool useMppDecode = false;
    std::atomic<bool> mppRequested_{false};
    std::atomic<bool> mppEffective_{false};
    std::atomic<std::uint64_t> mppDecodedFrames_{0};

    // Mock state tracking
    std::atomic<MockState> mockState{ MockState::NONE };
    int64_t mockLoadTimeoutMs = 30000;
    std::string mockFilePath;
    std::string lastMockRejectReason_;
    mutable std::mutex mockMetaMutex_;
};

} // namespace rk_core
