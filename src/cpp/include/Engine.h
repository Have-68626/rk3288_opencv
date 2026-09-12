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
#include "FrameTransform.h"
#include "pipeline/TrackCoordinator.h"
#include "pipeline/ResultPublisher.h"
#include "pipeline/PerfReporter.h"
#include "pipeline/RealtimeFacePipeline.h"  // 任务12：实时 SCRFD/ArcFace 管线接入
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rk_core {

/**
 * #1 注册采集逐帧样本。
 *
 * 仅当注册采集模式开启时由实时路径（run()/processFrameOnce 线程）对每个检测脸产出；
 * 承载 Java EnrollmentSession.FrameSample 所需的全部数值字段。
 *
 * 安全红线：embedding 仅经内存/回调传递，**严禁**写入日志或任何字符串；调用方用毕应清零。
 */
struct EnrollmentCaptureSample {
    std::vector<float> embedding;   // 512 维（来自实时管线 IRealtimeEmbedder）
    cv::Rect bbox;                  // 统一变换后帧坐标（右/下为开区间）
    int frameWidth = 0;
    int frameHeight = 0;
    float detScore = 0.0f;          // 检测置信度
    int faceCount = 0;              // 本帧后处理后检测脸总数（>=1）
    float grayMean = 0.0f;          // 对齐人脸灰度均值（0..255）
    float grayStd = 0.0f;           // 对齐人脸灰度总体标准差
    float laplacianVariance = 0.0f;  // Laplacian(4 邻域) 响应方差（清晰度）
    float areaRatio = 0.0f;         // 人脸框面积 / 帧面积
    bool hasLandmarks = false;
    std::array<float, 10> landmarks{};  // 5 点 (x,y) 扁平：左眼、右眼、鼻、左嘴角、右嘴角
};

using EnrollmentCaptureCallback = std::function<void(const EnrollmentCaptureSample&)>;

class Engine {
public:
    Engine();
    explicit Engine(std::unique_ptr<VideoManager> vm,
                    std::unique_ptr<BioAuth> ba,
                    std::unique_ptr<EventManager> em);
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
     * @brief 设置输入源是否已镜像（通常来自相机元数据 mirrored）。
     * 与 setFlip 共同决定实际水平变换：effective = input XOR target。
     */
    void setInputMirroredX(bool mirrored);

    /**
     * @brief 当前 frame transform generation（仅配置变化递增，供状态区诊断展示）。
     */
    uint64_t getFlipGeneration() const { return flipGeneration_.load(); }

    /**
     * @brief 镜像翻转应用（独立纯逻辑，供单测验证不修改输入）。
     * @return 翻转后的新 Mat；无翻转时返回输入引用。
     * WHY 独立：preprocessFrame 原先对共享缓冲原地 cv::flip，渲染线程读取时
     * 相位漂移导致画面按拍频来回镜像（RK3288 高频翻转回归）；提取为纯函数后
     * 输出新 Mat，输入 frame 保持不变，发布缓冲稳定。
     */
    static cv::Mat applyFlip(const cv::Mat& frame, bool flipX, bool flipY);

    /**
     * @brief 在帧上绘制人脸跟踪框与标签（认证绿框/未认证红框）。
     * WHY 独立：三层重构（029e43f）删除内联 drawTracks 后运行时无任何检测框；
     * 提取为纯函数供 renderResults 调用并可单测验证绘制。
     */
    static void drawTracks(cv::Mat& frame, const std::vector<pipeline::TrackView>& tracks);

    // ── 任务12：实时路径（RealtimeFacePipeline）接入 ──

    /**
     * @brief 注入实时推理管线（SCRFD 检测 + ArcFace 识别 + 图库检索）。
     *
     * 语义：
     * - 注入非空即启用实时路径，此后 processFrame 不再调用 BioAuth::verifyMulti；
     * - 注入 nullptr 恢复 BioAuth 兼容路径（Windows/旧 CLI/兼容测试）；
     * - Android 平台在构造时默认创建并启用（见 Engine::Engine），宿主可再次注入
     *   带真实 manifest 图库的管线替换之。
     * 线程安全：可与 run() 并发调用（内部加锁取共享指针）。
     */
    void setRealtimePipeline(std::shared_ptr<RealtimeFacePipeline> pipeline);

    /** 是否处于实时路径（已注入/启用管线）。 */
    bool hasRealtimePipeline() const { return realtimeEnabled_.load(); }

    /** 实时管线当前快照代次（无管线为 0），供状态展示。 */
    uint64_t getRealtimeGeneration() const;

    /**
     * @brief 最近一次实时路径结果 reason（契约码；成功为 "ok"）。
     * 模型缺失时为 "missing_model"（显式暴露，禁止静默回退 LBPH）。
     */
    std::string getRealtimeReason() const;

    /**
     * @brief 运行时更新实时图库（图库变化时由 JNI bridge_replaceGallery 调用）。
     *
     * WHY 独立接缝：RealtimePipelineSnapshot 为不可变对象，且其替换 API 不对 Engine 暴露，
     * 故由 Engine 持有一份「图库覆盖」不可变快照，身份匹配时优先于快照自带 gallery——
     * 等价于在 Engine 侧重建了快照的图库字段，避免为图库变更触发整条 reload（含基准）。
     * 传入 nullptr 表示清除覆盖，回退到快照自带图库。
     * 线程安全：可与 run() 并发调用（内部加锁取共享指针；传入对象为 const，读侧无数据竞争）。
     */
    void updateRealtimeGallery(std::shared_ptr<const FaceSearchLinearIndex> gallery);

    /**
     * @brief 当前实时快照是否 effective（模型/自检/基准全部通过）。
     * 供 JNI getRuntimeStatus 暴露 effective 字段，避免 UI 仅凭 profileLoaded 误报就绪。
     */
    bool getRealtimeEffective() const;

    /**
     * @brief 当前对外跟踪视图（已确认轨迹）的线程安全快照。
     * 与发布渲染帧同源，供 JNI 状态展示与单元测试断言。
     */
    std::vector<pipeline::TrackView> getTrackViews() const;

    // ── #1 注册采集模式（逐帧 embedding + 质量指标回调） ──

    /**
     * @brief 开启/关闭注册采集模式（无锁快速分支）。
     *
     * 关闭时会等待**其他线程**的在途回调返回后才返回（见 enrollmentCaptureCv_），调用方
     * 可据此安全释放在回调中使用的资源（如 JNI 全局引用）。若本线程正身处回调内
     * （回调同步重入关闭），不会自等——自等必然死锁；此时该回调返回后不再触达被释放资源。
     * 线程安全：可与 run() 并发调用。
     */
    void setEnrollmentCapture(bool enabled);

    /** 是否处于注册采集模式（原子读）。 */
    bool isEnrollmentCapture() const { return enrollmentCaptureEnabled_.load(); }

    /**
     * @brief 注册逐帧采集回调（可传 nullptr 清除）。
     *
     * 回调由 run()/processFrameOnce 线程在采集模式开启时逐脸调用；实现须线程安全且
     * 快速返回（阻塞会拖慢实时帧）。回调在**锁外**调用（锁内仅复制快照），故回调内
     * 同步重入 setEnrollmentCapture 不会造成同线程自死锁。严禁在回调内记录 embedding。
     */
    void setEnrollmentCaptureCallback(EnrollmentCaptureCallback callback);

    /**
     * @brief 灰度/清晰度质量指标（纯函数，语义对齐 EnrollmentSession.ImageMetrics）。
     * @param faceBgr 对齐后人脸图（BGR 或单通道灰度均可）
     * @param[out] grayMean 灰度均值；grayStd 总体标准差；laplacianVariance 4 邻域响应方差
     * @return 输入非空且为 8U/连续可计算时为 true；否则输出清零并返回 false
     * 只读输入，不修改；Laplacian 仅统计内部像素（与 Java 侧完全一致的公式）。
     */
    static bool computeEnrollmentQuality(const cv::Mat& faceBgr, float& grayMean,
                                         float& grayStd, float& laplacianVariance);

    /**
     * @brief 同步处理单帧：流程与 run() 内部单帧一致（预处理→检测/识别→跟踪→渲染）。
     *
     * 供宿主同步驱动与单元测试；不可与 run() 并发调用（会并发写
     * processedFrame_/currentTracks_ 等非线程安全状态）。
     */
    bool processFrameOnce(const cv::Mat& rawFrame, double decodeMs = 0.0);

    /**
     * @brief 实时检测框统一后处理（纯函数，任务12 可测接缝）。
     *
     * 入参 frame 已是「统一变换后」的图像，boxes 即该坐标系检测框（解码器已完成
     * letterbox 反算）；返回后处理后的 DetectedFace：丢弃非法框、裁剪到画面、
     * IoU NMS + 包含框抑制。关键：包含框抑制确保同一张脸的局部/嵌套框只保留一个，
     * 避免下游 TrackCoordinator 因两个框各建轨迹而产生「第二身份」。
     * 不修改 frame（只读 cols/rows）。
     */
    static std::vector<pipeline::DetectedFace> postProcessRealtimeBoxes(
        const cv::Mat& frame, const std::vector<RealtimeFaceBox>& boxes,
        float nmsIouThreshold = 0.40f);

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

    // === processFrame 子方法（P2.1）===
    bool processFrame(const cv::Mat& rawFrame, double decodeMs);
    bool preprocessFrame(const cv::Mat& frame);
    void trackFaces(const std::vector<pipeline::DetectedFace>& faces, bool detectionRan);
    void renderResults();
    void collectStats();

    // === 任务12 实时路径子方法 ===
    // 实时路径单帧：检测→（可选身份）→跟踪→渲染。processedFrame_ 已统一变换。
    bool processFrameRealtime(double decodeMs);
    // 实时检测：成功 true 且 detectionRan=true；模型/管线不可用 false（reason 已记录）。
    bool runRealtimeDetect(const cv::Mat& frame, std::vector<pipeline::DetectedFace>& outFaces,
                           bool& detectionRan);
    // 身份匹配（可选增强）：快照含 gallery+embedder 时对齐→嵌入→检索并回填身份。
    void assignRealtimeIdentities(const cv::Mat& frame,
                                  std::vector<pipeline::DetectedFace>& faces,
                                  const RealtimePipelineSnapshot& snap);
    void setRealtimeReason(const std::string& reason);

    // #1 注册采集：对每个已后处理检测脸对齐→嵌入→算质量指标→逐个回调。
    // 仅在采集模式开启时由 runRealtimeDetect 调用；关闭时零调用。
    void captureEnrollmentFaces(const cv::Mat& frame,
                                const std::vector<pipeline::DetectedFace>& faces,
                                const RealtimePipelineSnapshot& snap);

    // === 管线组件 ===
    std::unique_ptr<VideoManager> videoManager_;
    std::unique_ptr<MotionDetector> motionDetector_;
    std::unique_ptr<BioAuth> bioAuth_;
    std::unique_ptr<EventManager> eventManager_;
    std::unique_ptr<pipeline::TrackCoordinator> trackCoordinator_;
    std::unique_ptr<pipeline::ResultPublisher> publisher_;
    std::unique_ptr<pipeline::PerfReporter> perfReporter_;

    // 任务12：注入式实时管线（Android 默认创建；nullptr = 走 BioAuth 兼容路径）。
    // realtimeMu_ 保护共享指针的注入/读取；realtimeEnabled_ 为无锁快速分支判断。
    std::shared_ptr<RealtimeFacePipeline> realtimePipeline_;
    mutable std::mutex realtimeMu_;
    std::atomic<bool> realtimeEnabled_{false};
    std::string realtimeReason_;
    mutable std::mutex realtimeReasonMu_;

    // 图库覆盖快照：replaceGallery 成功后的不可变索引，身份匹配优先于快照自带 gallery。
    // 独立锁，避免与 realtimeMu_ 的注入路径互相阻塞。
    std::shared_ptr<const FaceSearchLinearIndex> realtimeGallery_;
    mutable std::mutex realtimeGalleryMu_;

    // #1 注册采集：enabled 为原子快速分支；回调在锁内复制快照、锁外调用，避免回调同步重入
    // setEnrollmentCapture 时同线程二次加锁自死锁。关闭时经 enrollmentCaptureCv_ 等待其他
    // 线程在途回调归零，故跨线程 setEnrollmentCapture(false) 返回后保证不再有在途回调，
    // 调用方（JNI 侧）可安全释放回调使用的全局引用。
    std::atomic<bool> enrollmentCaptureEnabled_{false};
    EnrollmentCaptureCallback enrollmentCaptureCb_;
    mutable std::mutex enrollmentCaptureMu_;
    std::condition_variable enrollmentCaptureCv_;
    std::size_t enrollmentCaptureInFlight_ = 0;  // 在途回调计数，受 enrollmentCaptureMu_ 保护

    // External frame input
    std::unique_ptr<FrameInputChannel> externalInput_;
    std::atomic<bool> externalInputEnabled_{false};

    // === 运行时状态（最小化）===
    // 初始化互斥锁：并发 initialize 串行化；初始化成功后才一次性提交 initialized_
    std::mutex initMu_;
    std::atomic<bool> isRunning_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<MonitoringMode> currentMode_{MonitoringMode::CONTINUOUS};
    std::atomic<bool> initCancelRequested_{false};

    // Preprocess 参数
    std::atomic<bool> flipXEnabled_{false};
    std::atomic<bool> flipYEnabled_{false};
    std::atomic<bool> inputMirroredX_{false};
    // generation：仅变换配置变化时递增（FrameTransform 语义固定，不由检测改变）
    std::atomic<uint64_t> flipGeneration_{0};

    // 节流
    std::atomic<InferenceThrottleMode> detThrottleMode_{InferenceThrottleMode::Off};
    std::atomic<int> detIntervalMs_{kDetectionIntervalDefaultMs};
    std::atomic<long long> lastDetStartMs_{0};
    std::atomic<InferenceThrottleMode> recThrottleMode_{InferenceThrottleMode::Off};
    std::atomic<int> recIntervalMs_{kRecognitionIntervalDefaultMs};
    std::atomic<long long> lastRecStartMs_{0};

    // === processFrame 子方法间共享状态 ===
    cv::Mat processedFrame_;
    // currentTracks_ 由推理线程写、getTrackViews()/渲染读，统一由 tracksMu_ 保护
    mutable std::mutex tracksMu_;
    std::vector<pipeline::TrackView> currentTracks_;
    pipeline::PerfStats currentStats_;

    // 其他
    std::string storagePath_;
    int maxFrames_ = 0;
};

} // namespace rk_core
