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
#include "FrameInputChannel.h"
#include "Types.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <mutex>
#include <vector>

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
     * @brief 启用/禁用外部帧输入通道（旁路 VideoManager）。
     *
     * 说明（JNI 边界/生命周期）：
     * - 启用后，Engine 的主循环会优先消费外部帧通道里的帧；
     * - 外部帧的数据应由 Native 持有（例如 JNI 层做拷贝后 push），避免 Java 侧复用 Buffer 造成悬挂指针。
     */
    void setExternalInputEnabled(bool enabled);

    /**
     * @brief 配置外部帧通道背压策略。
     * @param mode LatestOnly=仅保留最新帧；BoundedQueue=有限队列丢最旧。
     * @param capacity 队列上限（LatestOnly 下会被强制为 1）。
     */
    void configureExternalInput(FrameBackpressureMode mode, std::size_t capacity);

    /**
     * @brief 推入一帧外部输入（通常由 JNI 线程调用）。
     * @return true 表示已进入通道；false 表示 Engine 未初始化或未启用外部输入。
     */
    bool pushExternalFrame(ExternalFrame frame);

    /**
     * @brief Get the latest processed frame for rendering.
     * @param outFrame Output matrix.
     * @return true if a new frame is available.
     */
    bool getRenderFrame(cv::Mat& outFrame);
    bool getRenderFrame(cv::Mat& outFrame, uint64_t& outSeq);
    void requestCancelInit();
    void clearCancelInit();
    void setFlip(bool flipX, bool flipY);

private:
    void processFrame(cv::Mat& frame, double decodeMs);
    void handleAbnormalEvent(const std::string& type, const std::string& desc, const cv::Mat& evidence);

    std::unique_ptr<VideoManager> videoManager;
    std::unique_ptr<MotionDetector> motionDetector;
    std::unique_ptr<BioAuth> bioAuth;
    std::unique_ptr<EventManager> eventManager;

    std::atomic<bool> isRunning;
    std::atomic<MonitoringMode> currentMode;
    std::atomic<bool> externalInputEnabled;
    std::atomic<bool> initCancelRequested{false};
    std::atomic<bool> flipXEnabled{false};
    std::atomic<bool> flipYEnabled{false};
    
    // Rendering
    cv::Mat renderFrame;
    uint64_t renderFrameSeq = 0;
    std::mutex renderMutex;

    // Performance stats
    int frameCount;
    long long lastStatTime;
    int totalFrames = 0;
    int maxFrames = 0;

    struct FramePerfStats {
        double decodeMs = 0.0;
        double preMs = 0.0;
        double inferMs = 0.0;
        double postMs = 0.0;
        double renderMs = 0.0;
        long long rssBytes = 0;
    };
    std::vector<FramePerfStats> perfHistory;

    std::string storagePath;
    std::function<void(std::string)> onResultCallback;

    std::unique_ptr<FrameInputChannel> externalInput;

    long long lastNoFaceMs = 0;
    long long lastUnknownMs = 0;
    long long lastVerifiedMs = 0;
    long long lastMultiMs = 0;

    struct FaceTrack {
        int trackId = 0;
        cv::Rect bbox;
        long long lastSeenMs = 0;

        std::string lastId;
        int lastIdStreak = 0;

        std::string stableId;
        float stableConfidence = 0.0f;
    };

    std::vector<FaceTrack> faceTracks;
    int nextTrackId = 1;

public:
    void setMaxFrames(int max) { maxFrames = max; }

    void setOnResultCallback(std::function<void(std::string)> callback) {
        onResultCallback = callback;
    }
};
