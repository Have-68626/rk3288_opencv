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
#include "pipeline/LogRateLimiter.h"  // noteEmbedError 的时间窗限频器（纯逻辑，零 OpenCV 依赖）
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
    // 析构：仅在启用测试接缝宏（RK_ENABLE_TEST_SEAMS）时声明为 virtual——该宏下 Engine 含
    // protected virtual 接缝（onBeforeRenderPublish），子类经基类指针删除需虚析构保证行为正确
    //（第四轮审查 M3）；生产构建**不定义**该宏 → Engine 无任何虚函数、无 vptr，对象布局稳定，
    // 析构恢复非虚，测试接缝不再改变 ABI（编译期开关，见 CMakeLists.txt 对 core_gtest_tests 的定义）。
#if defined(RK_ENABLE_TEST_SEAMS)
    virtual ~Engine();
#else
    ~Engine();
#endif

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
     * @return true 表示已进入通道；false 表示**外部输入未启用**（externalInputEnabled_ == false）。
     *         ⚠️ 本函数**不检查** initialized_：初始化状态不参与判据（原注释「未初始化或未启用」中
     *         的「未初始化」与实现不符，已订正）。
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
     * 换管线 = 换模型：新旧模型嵌入空间不同，旧模型下得出的 stableId 一律失效，故本方法除
     * 重置图库覆盖并递增图库代次外，还会在 tracksMu_ 下**撤销全部跟踪身份**（保留轨迹本体与
     * bbox）并递增 identityVersion_，使在途渲染在发布点作废旧身份（与 applyGalleryUpdate 的
     * 撤销语义一致；否则换模型后残留旧 id 会持续输出 VERIFIED，见上轮未覆盖项 B）。
     * 幂等（第四轮审查 M1）：仅当传入 shared_ptr 与当前持有者**指向不同对象**时才执行上述
     * 重置与撤销；同对象重复注入视为「未换模型」直接返回——否则每次重复激活都会抹掉在途稳定
     * 身份、需重新累计 stableFrames、画面短暂回退 Unknown，并使紧随其后的 injectGalleryToEngine
     * 的 keepIds 无物可保（撤销先行）。「换模型必撤销」语义不变：换到新对象或启停
     *（null ↔ 非 null）仍完整撤销。
     * 线程安全：可与 run() 并发调用（内部加锁取共享指针）。
     * @return true = 已生效（注入/清除完成，含同对象幂等注入）；false = 被拒绝——仅发生在**结果
     *         回调内同步重入**时（本 API 会取 tracksMu_，见 FACE-PIPE-015 发射门重入检测）。
     */
    bool setRealtimePipeline(std::shared_ptr<RealtimeFacePipeline> pipeline);

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
     * @brief 图库变更的**原子提交**：在**同一 tracksMu_ 临界区**内完成「换图库覆盖 +
     *        按新图库有效身份精准撤销跟踪身份」。由 JNI bridge_replaceGallery 成功分支
     *        **单次调用**（取代此前「先精准撤销、再换库」的两次独立持锁调用）。
     *
     * WHY（第三波审查 #4 残留竞态）：两次独立调用之间存在可观测缝隙：引擎线程可在此缝隙内
     * 用**旧图库**跑完一帧——update() 见 stableId=="Unknown" 且该帧检测已认证时会**重新赋
     * 身份**，随后新图库（已无此人）生效，被持久保留 → 等同 #2 复发。合并为单一临界区后，
     * 「换库」与「撤销」之间不存在可观测中间态：任何 trackFaces/update 要么看到（旧库 + 旧
     * 身份），要么看到（新库 + 已撤销身份）。
     *
     * 读路径补充（关键评估结论）：assignRealtimeIdentities 读 realtimeGallery_ 走
     * realtimeGalleryMu_（**非** tracksMu_），且「读图库」与 trackFaces「提交轨迹」之间隔着
     * 嵌入计算——故单靠本合并仍有一个更小的残留窗口：某帧在换库前读到旧库（认证为已删者）、
     * 却在换库后才提交轨迹。为此引入 galleryGeneration_ 代次：换库时递增，assign 记录当帧读库
     * 代次，trackFaces 在 tracksMu_ 内比对，代次不一致即作废该帧认证结论，从而彻底闭合。
     *
     * @param gallery 新图库不可变索引（nullptr = 清除覆盖，回退到快照自带图库）
     * @param keepIds 本次提交后**仍有效**的身份集合（新图库 personIds）；未命中者身份清为
     *                "Unknown"（保留轨迹本体与 bbox，不丢框）
     * @return true = 已换库并精准撤销；false = 被拒绝——仅发生在**结果回调内同步重入**时（本 API 会
     *         取 tracksMu_，见 FACE-PIPE-015 发射门重入检测：从发射临界区内的回调同步回入本 API 会
     *         与撤销路径构成 ABBA 死锁，故拒绝而非挂死；该路径当前生产不可达，对现有使用者零行为变化）。
     * 线程安全：可与 run() 并发调用（内部持 tracksMu_，与 trackFaces 的 update() 互斥）。
     */
    bool applyGalleryUpdate(std::shared_ptr<const FaceSearchLinearIndex> gallery,
                            const std::vector<std::string>& keepIds);

    /**
     * @brief 当前实时快照是否 effective（模型/自检/基准全部通过）。
     * 供 JNI getRuntimeStatus 暴露 effective 字段，避免 UI 仅凭 profileLoaded 误报就绪。
     */
    bool getRealtimeEffective() const;

    /**
     * @brief 当前对外跟踪视图（已确认轨迹）的线程安全快照。
     * 与发布渲染帧同源，供 JNI 状态展示与单元测试断言。
     * @note 若在结果回调内同步重入（此时本线程持有 identityEmitMu_），返回**空快照**并记录 reason
     *       （见 FACE-PIPE-015），**不取 tracksMu_**——避免与撤销路径 ABBA 死锁。
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
     * @return false = 帧处理被拒绝——仅发生在**结果回调内同步重入**时（本 API 经 trackFaces/
     *         renderResults 会取 tracksMu_，见 FACE-PIPE-015 发射门重入检测）。
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
     * - 该接口只写入原子变量，不触碰 renderFrame / currentTracks_ 等非线程安全资源
     *   （原注释写的是 `faceTracks`——该成员已不存在，属重构后未同步的旧名，本批订正）；
     * - Engine 线程在每帧处理时读取原子配置，做到"低锁竞争"的实时生效。
     *
     * 行为约束：
     * - mode 支持：auto/manual/off（大小写不敏感），非法值会按 off 处理（parseInferenceThrottleMode）；
     * - intervalMs 会被钳制，但**两个字段的上界不同**（原注释统一写成
     *   [kInferenceIntervalMinMs, kInferenceIntervalMaxMs] 与实现不符，已订正）：
     *     · 检测间隔 → clampDetectionIntervalMs ⇒ [kDetectionIntervalMinMs, kDetectionIntervalMaxMs]
     *     · 识别间隔 → clampRecognitionIntervalMs ⇒ [kRecognitionIntervalMinMs, kRecognitionIntervalMaxMs]
     *   两处下界同为 80，上界分别是 500 / 2000；故同一 intervalMs 写入后二者可能不同值。
     *   本接口一次写入**两组**节流参数（检测 + 识别）；单独设置用 updateDetectionThrottle /
     *   updateRecognitionThrottle。
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

    // ⚠️ 如实标注：maxFrames_ 目前是**只写不读**的字段——Engine 内无任何读取方（run() 的主循环
    // 不含帧数上限判断），故本 setter 不产生任何效果。CLI 的 --max-frames 经 main.cpp 调用本
    // 接口，同样不生效。改动前请先确认该事实是否已变。
    void setMaxFrames(int max) { maxFrames_ = max; }

    void setOnResultCallback(std::function<void(std::string)> callback);

protected:
#if defined(RK_ENABLE_TEST_SEAMS)
    /**
     * @brief 发布前钩子（测试接缝，默认空实现）：renderResults 在「快照拷贝后、发布前」窗口内调用。
     *
     * WHY（第四轮审查 M3）：此前用公有 setRenderPublishBarrier 注入 std::function 成员充当该窗口的
     * 同步点；该成员由引擎线程无锁读、setter 写，任何运行期调用即构成数据竞争 UB，且是公有的误用面。
     * 改为 protected virtual 空钩子后：仅测试子类可覆写以确定性钉住该窗口（取代 sleep 竞态）。
     * 编译期开关：本宏由 CMake 对 `core_gtest_tests` 与 `win_unit_tests` **两个** target 定义 —— **本条
     * Engine 接缝**的消费者是 `core_gtest_tests`（该 target 编译 test_engine.cpp 的 BarrierEngine 与
     * Engine.cpp，宏一致无 ODR 问题）；`win_unit_tests` 也定义同一宏，但用于 WinJsonConfigStore 的接缝
     *（见 WinJsonConfig.h），与本条无关。生产构建不定义 → 无虚函数、无 vptr。
     * 调用时**不持任何锁**，覆写实现内可安全调用 applyGalleryUpdate 等会取 tracksMu_ 的公共 API。
     */
    virtual void onBeforeRenderPublish() {}

    /**
     * @brief 事件发射前钩子（测试接缝，默认空实现）：在**持有 identityEmitMu_** 的发射临界区内、
     * publisher_->publish 之前调用。
     *
     * WHY（第五轮审查 P1）：需要一个**确定性**窗口来验证「撤销无法插在最终校验与事件发射之间」。
     * 测试把引擎线程钉在此处（此时必已持 identityEmitMu_），再令另一线程调用 applyGalleryUpdate，
     * 断言其**在放行前无法返回**——缺该门时 applyGalleryUpdate 会立即返回（RED）。
     * 注意：本钩子在**持有 identityEmitMu_** 的状态下调用，覆写实现**不得**再取 identityEmitMu_
     *（std::mutex 非递归，必自死锁）。调用会取 tracksMu_ 的公开 API（run / applyGalleryUpdate /
     * setRealtimePipeline / getTrackViews / processFrameOnce —— 即上条列出的同一穷尽集）则已被
     * FACE-PIPE-015 重入检测**拒绝**（run 直接返回、其余返回 false / 空快照），不会死锁——本条
     * 重入用例正据此在同一线程内确定性地回归「拒绝而非挂死」。
     */
    virtual void onBeforePublishLocked() {}
#endif

private:
    /** Shared by both initialize() overloads: storage, cleanup, BioAuth */
    bool initCommon(const std::string& cascadePath, const std::string& storagePath);
    void performAccelSelfCheck();

    // === processFrame 子方法（P2.1）===
    bool processFrame(const cv::Mat& rawFrame, double decodeMs);
    bool preprocessFrame(const cv::Mat& frame);
    // 提交本帧跟踪：trackCoordinator_->update() + currentTracks_ 更新，持 tracksMu_。
    // frameGalleryGen：本帧身份匹配所用图库代次（实时路径传 frameGalleryGeneration_；
    //   兼容路径传 kNoGalleryGuard 表示不做代次作废）。若与当前 galleryGeneration_ 不一致，
    //   说明图库在「读库」与「提交」之间被替换，本帧据旧库得出的认证结论作废后再交 coordinator。
    void trackFaces(const std::vector<pipeline::DetectedFace>& faces, bool detectionRan,
                    uint64_t frameGalleryGen);
    void renderResults();
    void collectStats();

    // 精准撤销跟踪身份（keepIds 命中者保留，其余清为 "Unknown"）。**调用方须已持 tracksMu_**，
    // 保证与 trackFaces 的 update() 互斥；仅改身份、保留轨迹本体与 bbox（不丢框）。
    void revokeTrackIdentitiesLocked(const std::vector<std::string>& keepIds);

    // trackFaces 的哨兵参数：表示「不做图库代次作废」（兼容路径不经图库覆盖匹配）。
    static constexpr uint64_t kNoGalleryGuard = ~static_cast<uint64_t>(0);

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
    // 逐帧 × 每脸路径上的 embed / 图库检索失败：把差异化 err 记成**限频**的可观测日志。
    // ⚠️ 限频判据**只有时间窗**（与 setRealtimeReason 的「值变化才记」范式**不同**，勿照搬：那条
    // 范式对「多个不同文本交替出现」没有任何上界）⇒ 日志量有全局上界（每窗最多一条）；
    //   err 文本 / site 的变化**不参与**判据。窗口用 steady_clock（单调），墙钟回拨不会令其静默。
    // **刻意不复用** realtimeReason_：那是对外 reason 契约通道，写入逐脸错误会污染其语义。
    // err 文本**只进日志**，不外传 Java、不进 getRuntimeStatus。
    void noteEmbedError(const char* site, const std::string& err);

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

    // 逐脸 embed / 图库检索失败日志的时间窗限频器（判据只有时间，见 LogRateLimiter.h 的契约）。
    // 由 embedErrMu_ 串行化：判据与窗口推进必须在同一临界区内完成。
    pipeline::LogRateLimiter embedErrLimiter_{pipeline::kEmbedErrorLogWindowMs};
    std::mutex embedErrMu_;

    // 图库覆盖快照：replaceGallery 成功后的不可变索引，身份匹配优先于快照自带 gallery。
    // 独立锁，避免与 realtimeMu_ 的注入路径互相阻塞。
    std::shared_ptr<const FaceSearchLinearIndex> realtimeGallery_;
    mutable std::mutex realtimeGalleryMu_;

    // 图库代次：任何对 realtimeGallery_ 的替换（applyGalleryUpdate / setRealtimePipeline 重置）
    // 都在 realtimeGalleryMu_ 下原子递增。assignRealtimeIdentities 读库
    // 时把所用代次记入 frameGalleryGeneration_，trackFaces 在 tracksMu_ 内比对——若图库在「读库」
    // 与「提交轨迹」之间被替换，则该帧据旧库得出的认证结论作废（闭合 #4 残留竞态，见
    // applyGalleryUpdate 注释）。之所以用代次而非把读库纳入 tracksMu_：读库与提交之间隔着嵌入
    // 计算，无法在该锁内完成，唯代次比对能在提交点判定「读数已过期」。
    std::atomic<uint64_t> galleryGeneration_{0};
    uint64_t frameGalleryGeneration_{0};  // 仅引擎线程在单帧内 写(assign)→读(processFrameRealtime)

    // 身份版本号：任何**撤销 / 清空跟踪身份**的对外操作（applyGalleryUpdate、
    // setRealtimePipeline[仅管线真正变化时]）都在 tracksMu_ 内递增。renderResults 在锁内拷贝
    // currentTracks_ 的同时记录该版本，并在
    // **发布前**比对：若期间发生撤销，本次快照的稳定身份已失效，须清为 Unknown 后再发布——
    // 否则已撤销身份会随旧副本被画进 renderFrame 并作为 VERIFIED 发出（发布路径 P1 竞态：拷贝在
    // 锁内、绘制/发布在锁外）。与 galleryGeneration_ 的分工：后者闭合「读库 → 提交轨迹」窗口
    //（trackFaces 内比对），前者闭合「拷贝 → 绘制 → 发布」窗口（renderResults 内比对）。
    // 单调递增，仅在外部撤销身份时变化。
    std::atomic<uint64_t> identityVersion_{0};

    // 事件发射门（第五轮审查 P1）：序列化「身份撤销**返回**」与「事件发射」，补齐版本号单独
    // 无法闭合的最后一段窗口。
    //
    // 缺陷：renderResults 的最终版本校验与其后的 publisher_->publish() 之间仍有缝隙——JNI 线程可在
    // 缝隙内完成 applyGalleryUpdate 并**返回**，此后本帧仍按旧副本发出 VERIFIED <已删除的人>。
    // 仅靠「检后再发」无法闭合：检查与发射不是原子的。
    //
    // 规则（两侧缺一不可）：
    //   · renderResults 在**持本门**的临界区内完成「最终校验 + publish」；
    //   · revokeTrackIdentitiesLocked 先递增 identityVersion_（使在途帧的最终校验能看见），
    //     再取本门**等待在途发射结束**才返回（见其实现）。
    // 于是二者二选一：发射先入门 → 撤销等它结束才返回；撤销先递增 → 发射入门时校验必已失败、
    // 先清身份再发布。两条路径都保证「撤销返回后不再发出旧身份」。
    //
    // 锁序：撤销路径 tracksMu_ → identityEmitMu_；renderResults 取本门之前**已释放** tracksMu_，
    // 且持门期间不再取 tracksMu_（clearStableIdentities 为纯函数、publish 不触达 Engine 状态）
    // → 无环、无死锁。
    //
    // 门内会经 publish 调用结果回调（Android 侧为 JNI 上行调用）。当前 onNativeResult 仅
    // runOnUiThread 投递后立即返回，**不同步回入 Engine**，故无死锁。此前「回调不得同步回入」仅靠
    // 注释约束，无任何强制；现由 **FACE-PIPE-015 重入检测**升级为可执行约束：
    //   · renderResults 在持本门的临界区内经 RAII 置位 thread_local 标记（t_insideIdentityEmitGate，
    //     实现见 Engine.cpp）表示「本线程正处发射门内」；
    //   · 所有会取 tracksMu_ 的**公开入口**——run / processFrameOnce / applyGalleryUpdate /
    //     setRealtimePipeline / getTrackViews（A1 审计枚举 Engine.cpp 全部加锁点后得出的穷尽集；
    //     A2 审计独立复核同一结论）——均入口即检查：命中重入 → 立即返回并记录固定 reason
    //     "re-entered from result callback while holding identity emit gate"，绝不进入取锁流程。
    //
    // ⚠️ **检测的边界（不得读成无条件保证）**：
    //   · 只覆盖**同一线程**的回入——标记是 thread_local 且只在 renderResults 内置位。**跨线程**的
    //     同步回入（回调把工作丢给另一线程并阻塞等待，如 `std::thread t(...); t.join();`）**绕开检测**，
    //     仍构成真实 ABBA 死锁。该形态当前生产不可达，但也**未**被本机制拦住。
    //   · 标记是**进程级 thread_local**、不是 per-Engine：同线程上 Engine A 处于发射门内时，调用
    //     Engine B 的上述 API 会被**误拒**。生产为单 Engine，无实际影响。
    //   · 拒绝的返回值并非都为调用方新增的可区分信号：applyGalleryUpdate / setRealtimePipeline 的
    //     false 只可能来自本拒绝（无歧义）；但 getTrackViews 的 {} 与「确实无已确认轨迹」同值、
    //     processFrameOnce 的 false 与「preprocessFrame 失败」同值，唯一诊断通道是那条 warn 日志。
    //   · **刻意不加 `[[nodiscard]]`**（子代理4 建议过）：拒绝对调用方是 **best-effort** 语义——防死锁
    //     这个主要收益与「调用方是否读返回值」无关（拒绝发生在取锁之前），而 4 个生产调用点本就有意
    //     忽略；加 `[[nodiscard]]` 只会逼它们写 `(void)` 噪声，且与本条「诊断通道是日志」的定性相矛盾。
    //
    // 于是「回调**同线程**同步回入」不再构成 ABBA 死锁，而是立刻得到明确错误。该路径当前**生产不可达**
    //（Android onNativeResult 仅投递、不同步回入；src/win/ 未注册结果回调），故对现有使用者**零行为变化**。
    //
    // 回调实现契约（FACE-PIPE-018：把上面那句隐含约定升格为**成文约束**）。门内会经 publish 同步调用
    // 结果回调（Android 侧即 JNI 上行 sendRecognitionResult → MainActivity.onNativeResult），且该回调
    // 运行在**持本门**的临界区内。故回调实现必须遵守：
    //   · **仅投递**——如 runOnUiThread 投递后立即返回；**不得**同步回入 Engine 任何会取 tracksMu_ /
    //     identityEmitMu_ 的公开 API（穷尽集见上），也**不得**在回调内长时间持锁或阻塞等待其它线程
    //     （阻塞等待会把本临界区的持锁时间拉长到不可控）。
    //   · 该约束的**强制执行点**是 FACE-PIPE-015 的重入检测（非仅靠注释自觉）：renderResults 在持门
    //     临界区内经 RAII `IdentityEmitGateScope`（定义于 Engine.cpp 匿名 namespace）置位进程级
    //     thread_local `t_insideIdentityEmitGate`；上述 5 个公开入口内联调用
    //     `reenteredFromIdentityEmitGate` 检测，命中即以固定 reason
    //     "re-entered from result callback while holding identity emit gate" 记录并**拒绝执行**——
    //     于是同步回入得到的是明确错误，而非 ABBA 死锁。
    //     ⚠️ 本条目**刻意以符号名而非行号作锚点**：行号在重构中极易漂移失准，本仓已有
    //     「注释两次因行号漂移而失真」的先例（见 native_face_bridge.cpp 中同类说明）。
    //   · 时间上**有界但非强制上界**：该上行受结果节流，观测量级为每秒 ≤1–1.5 次、单次 0.1–1 ms。
    //     节流参数可变，**不得**据此假定回调耗时不会增长；此处的「有界」纯属观测，不是硬保证。
    mutable std::mutex identityEmitMu_;

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
    // generation：仅变换配置变化时递增（FrameTransform 语义固定，不由检测改变）。
    // ⚠️ 与 FrameTransformConfig::generation **不是同一个东西**：后者生产侧恒 0（preprocessFrame
    //    构造 cfg 时不填充，也无人读取，见 FrameTransform.h 文件头）；本字段才是真正在用的那个。
    std::atomic<uint64_t> flipGeneration_{0};

    // 节流
    std::atomic<InferenceThrottleMode> detThrottleMode_{InferenceThrottleMode::Off};
    std::atomic<int> detIntervalMs_{kDetectionIntervalDefaultMs};
    // ⚠️ 如实标注（同 maxFrames_ 的处理方式）：lastDetStartMs_ / lastRecStartMs_ 目前是**从未被
    // 写入也从未被读取**的残留字段——全仓无任何引用（节流的时间推进不靠它们）。保留仅为不改布局，
    // 不得据此推断存在「上次检测/识别起始时刻」的记账逻辑。
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
