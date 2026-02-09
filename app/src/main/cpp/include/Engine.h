/**
 * @file Engine.h
 * @brief Main application orchestrator.
 * 
 * Integrates VideoManager, MotionDetector, BioAuth, and EventManager.
 * Implements the main state machine and business logic loop.
 */
#pragma once

#include "VideoManager.h"
#include "MotionDetector.h"
#include "BioAuth.h"
#include "EventManager.h"
#include "Types.h"
#include <atomic>
#include <memory>
#include <functional>

class Engine {
public:
    Engine();
    ~Engine();

    /**
     * @brief Initializes the engine and all sub-modules.
     * @param cameraId The camera device index to open.
     * @param cascadePath Path to HAAR/LBP cascade file.
     * @param storagePath Path to application private storage.
     * @return true if successful.
     */
    bool initialize(int cameraId, const std::string& cascadePath, const std::string& storagePath);

    /**
     * @brief Initializes the engine with a mock file source.
     * @param filePath Path to image or video file.
     * @param cascadePath Path to HAAR/LBP cascade file.
     * @param storagePath Path to application private storage.
     * @return true if successful.
     */
    bool initialize(const std::string& filePath, const std::string& cascadePath, const std::string& storagePath);

    /**
     * @brief Starts the main processing loop.
     * Blocks until stop() is called.
     */
    void run();

    /**
     * @brief Signals the engine to stop.
     */
    void stop();

    /**
     * @brief Switch video monitoring mode.
     */
    void setMode(MonitoringMode mode);

    /**
     * @brief Get the latest processed frame for rendering.
     * @param outFrame Output matrix.
     * @return true if a new frame is available.
     */
    bool getRenderFrame(cv::Mat& outFrame);

private:
    void processFrame(const cv::Mat& frame);
    void handleAbnormalEvent(const std::string& type, const std::string& desc, const cv::Mat& evidence);

    std::unique_ptr<VideoManager> videoManager;
    std::unique_ptr<MotionDetector> motionDetector;
    std::unique_ptr<BioAuth> bioAuth;
    std::unique_ptr<EventManager> eventManager;

    std::atomic<bool> isRunning;
    std::atomic<MonitoringMode> currentMode;
    
    // Rendering
    cv::Mat renderFrame;
    std::mutex renderMutex;

    // Performance stats
    int frameCount;
    long long lastStatTime;

    std::string storagePath;
    std::function<void(std::string)> onResultCallback;

public:
    void setOnResultCallback(std::function<void(std::string)> callback) {
        onResultCallback = callback;
    }
};
