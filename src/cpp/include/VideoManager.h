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

    // Mock state tracking
    std::atomic<MockState> mockState{ MockState::NONE };
    int64_t mockLoadTimeoutMs = 30000;
    std::string mockFilePath;
    std::string lastMockRejectReason_;
    mutable std::mutex mockMetaMutex_;
};

} // namespace rk_core
