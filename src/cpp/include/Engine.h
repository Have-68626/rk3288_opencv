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
#include "InferenceThrottle.h"
#include "pipeline/TrackCoordinator.h"
#include "pipeline/ResultPublisher.h"
#include "pipeline/PerfReporter.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <mutex>
#include <unordered_map>
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

    /**
     * @brief 运行时更新推理节流参数（通常由 JNI/配置线程调用）。
     *
     * 并发/线程安全：
     * - 该接口只写入原子变量，不触碰 renderFrame/faceTracks 等非线程安全资源；
     * - Engine 线程在每帧处理时读取原子配置，做到"低锁竞争"的实时生效。
     *
     * 行为约束：
     * - mode 支持：auto/manual/off（大小写不敏感），非法值会按 off 处理；
     * - intervalMs 会被钳制到 [kInferenceIntervalMinMs, kInferenceIntervalMaxMs]，避免 UI/调用方误传导致异常行为。
     */
    void updateInferenceThrottle(const std::string& mode, int intervalMs);
    InferenceThrottleMode getInferenceThrottleMode() const;
    int getInferenceIntervalMs() const;

    void updateDetectionThrottle(const std::string& mode, int intervalMs);
    InferenceThrottleMode getDetectionThrottleMode() const;
    int getDetectionIntervalMs() const;

    void updateRecognitionThrottle(const std::string& mode, int intervalMs);
    InferenceThrottleMode getRecognitionThrottleMode() const;
    int getRecognitionIntervalMs() const;

    void setMaxFrames(int max) { maxFrames_ = max; }

    void setOnResultCallback(std::function<void(std::string)> callback);

private:
    /** Shared by both initialize() overloads: storage, cleanup, BioAuth */
    bool initCommon(const std::string& cascadePath, const std::string& storagePath);
    void performAccelSelfCheck();

    // === 管线组件 ===
    std::unique_ptr<VideoManager> videoManager_;
    std::unique_ptr<MotionDetector> motionDetector_;
    std::unique_ptr<BioAuth> bioAuth_;
    std::unique_ptr<EventManager> eventManager_;
    std::unique_ptr<pipeline::TrackCoordinator> trackCoordinator_;
    std::unique_ptr<pipeline::ResultPublisher> publisher_;
    std::unique_ptr<pipeline::PerfReporter> perfReporter_;

    // External frame input
    std::unique_ptr<FrameInputChannel> externalInput_;
    std::atomic<bool> externalInputEnabled_{false};

    // === 运行时状态（最小化）===
    std::atomic<bool> isRunning_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<MonitoringMode> currentMode_{MonitoringMode::CONTINUOUS};
    std::atomic<bool> initCancelRequested_{false};

    // Preprocess 参数
    std::atomic<bool> flipXEnabled_{false};
    std::atomic<bool> flipYEnabled_{false};

    // 节流
    std::atomic<InferenceThrottleMode> detThrottleMode_{InferenceThrottleMode::Off};
    std::atomic<int> detIntervalMs_{kDetectionIntervalDefaultMs};
    std::atomic<long long> lastDetStartMs_{0};
    std::atomic<InferenceThrottleMode> recThrottleMode_{InferenceThrottleMode::Off};
    std::atomic<int> recIntervalMs_{kRecognitionIntervalDefaultMs};
    std::atomic<long long> lastRecStartMs_{0};

    // 其他
    std::string storagePath_;
    int maxFrames_ = 0;
};
