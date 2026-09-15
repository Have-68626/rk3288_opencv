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
    // 虚析构：Engine 现含 protected virtual 接缝（onBeforeRenderPublish）供测试子类覆写，
    // 声明为 virtual 保证经基类指针删除子类对象时行为正确（第四轮审查 M3）。
    virtual ~Engine();

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
     * @brief 运行时更新实时图库（单项 API；**不含**身份撤销）。
     *
     * 注：bridge_replaceGallery 现改用 applyGalleryUpdate（换库 + 精准撤销同一临界区原子完成，
     * 第三波审查 #4）。本单项 API 保留供测试/其他场景，且每次调用会递增图库代次
     *（galleryGeneration_），使在途帧据旧库得出的认证结论在提交点作废（见 applyGalleryUpdate）。
     *
     * WHY 独立接缝：RealtimePipelineSnapshot 为不可变对象，且其替换 API 不对 Engine 暴露，
     * 故由 Engine 持有一份「图库覆盖」不可变快照，身份匹配时优先于快照自带 gallery——
     * 等价于在 Engine 侧重建了快照的图库字段，避免为图库变更触发整条 reload（含基准）。
     * 传入 nullptr 表示清除覆盖，回退到快照自带图库。
     * 线程安全：可与 run() 并发调用（内部加锁取共享指针；传入对象为 const，读侧无数据竞争）。
     */
    void updateRealtimeGallery(std::shared_ptr<const FaceSearchLinearIndex> gallery);

    /**
     * @brief 撤销全部跟踪身份（清空轨迹），由 JNI bridge_replaceGallery 成功分支调用。
     *
     * WHY：人员删除 / 图库重建后，旧轨迹内部仍持有被删除者的 stableId；TrackCoordinator
     * 只在非认证帧「不覆盖」而非清除，故旧 stableId 会保留到该人离开画面（连续 2 个实际
     * 检测缺失）才消失，期间 ResultPublisher 持续回调 "VERIFIED <id> ..."，即删除的人仍
     * 被持续判为已认证（第三波审查 #2）。此处清空轨迹的**真相持有者**（trackCoordinator_
     * 内部 tracks_，其 view.stableId 才是身份来源；currentTracks_ 仅为逐帧派生的对外快照），
     * 并一并清空 currentTracks_ 使对外视图立即失效；新帧须重新累计确认帧数后才会再次输出。
     * 线程安全：可与 run() 并发调用（内部持 tracksMu_，与 trackFaces 的 update() 互斥）。
     */
    void resetTrackIdentities();

    /**
     * @brief 按图库有效身份**精准撤销**跟踪身份（第三波审查 #1：取代过宽的全清）。
     *
     * WHY：图库变更（人员删除 / 注册提交）后，只需撤销「已不在新图库中」的身份。旧实现
     * resetTrackIdentities() 无条件清空全部轨迹，导致多人场景删除/注册**一人**时瞬时抹掉
     * **其他人**的稳定身份，须重新累计 stableFrames 才重新输出（丢身份、画面抖动）。
     * 本接口仅把 stableId **不在** keepIds 中的轨迹身份清为 "Unknown"（保留轨迹本体与 bbox，
     * 不丢框），keepIds 中的身份原样保留。空图库（删除末位人员）→ keepIds 为空 → 全部清为
     * "Unknown"（与 resetTrackIdentities 的全清语义等价，但不丢框）。
     * keepIds 来源：JNI bridge_replaceGallery 本次提交的 personIds 数组（即新图库有效身份）。
     * 线程安全：可与 run() 并发调用（内部持 tracksMu_，与 trackFaces 的 update() 互斥）。
     */
    void resetTrackIdentitiesExcept(const std::vector<std::string>& keepIds);

    /**
     * @brief 图库变更的**原子提交**：在**同一 tracksMu_ 临界区**内完成「换图库覆盖 +
     *        按新图库有效身份精准撤销跟踪身份」。由 JNI bridge_replaceGallery 成功分支
     *        **单次调用**（取代此前的 resetTrackIdentitiesExcept 后再 updateRealtimeGallery
     *        两次独立持锁调用）。
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
     * 线程安全：可与 run() 并发调用（内部持 tracksMu_，与 trackFaces 的 update() 互斥）。
     */
    void applyGalleryUpdate(std::shared_ptr<const FaceSearchLinearIndex> gallery,
                            const std::vector<std::string>& keepIds);

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

protected:
    /**
     * @brief 发布前钩子（测试接缝，默认空实现）：renderResults 在「快照拷贝后、发布前」窗口内调用。
     *
     * WHY（第四轮审查 M3）：此前用公有 setRenderPublishBarrier 注入 std::function 成员充当该窗口的
     * 同步点；该成员由引擎线程无锁读、setter 写，任何运行期调用即构成数据竞争 UB，且是公有的误用面。
     * 改为 protected virtual 空钩子后：生产路径恒为空（一次虚调用，无函数对象/堆开销，无可被误调的
     * 公有入口）；仅测试子类可覆写以确定性钉住该窗口（取代 sleep 竞态）。
     * 调用时**不持任何锁**，覆写实现内可安全调用 applyGalleryUpdate 等会取 tracksMu_ 的公共 API。
     */
    virtual void onBeforeRenderPublish() {}

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

    // 图库代次：任何对 realtimeGallery_ 的替换（applyGalleryUpdate / updateRealtimeGallery /
    // setRealtimePipeline 重置）都在 realtimeGalleryMu_ 下原子递增。assignRealtimeIdentities 读库
    // 时把所用代次记入 frameGalleryGeneration_，trackFaces 在 tracksMu_ 内比对——若图库在「读库」
    // 与「提交轨迹」之间被替换，则该帧据旧库得出的认证结论作废（闭合 #4 残留竞态，见
    // applyGalleryUpdate 注释）。之所以用代次而非把读库纳入 tracksMu_：读库与提交之间隔着嵌入
    // 计算，无法在该锁内完成，唯代次比对能在提交点判定「读数已过期」。
    std::atomic<uint64_t> galleryGeneration_{0};
    uint64_t frameGalleryGeneration_{0};  // 仅引擎线程在单帧内 写(assign)→读(processFrameRealtime)

    // 身份版本号：任何**撤销 / 清空跟踪身份**的对外操作（applyGalleryUpdate、
    // resetTrackIdentities、resetTrackIdentitiesExcept、setRealtimePipeline[仅管线真正变化时]）
    // 都在 tracksMu_ 内递增。renderResults 在锁内拷贝 currentTracks_ 的同时记录该版本，并在
    // **发布前**比对：若期间发生撤销，本次快照的稳定身份已失效，须清为 Unknown 后再发布——
    // 否则已撤销身份会随旧副本被画进 renderFrame 并作为 VERIFIED 发出（发布路径 P1 竞态：拷贝在
    // 锁内、绘制/发布在锁外）。与 galleryGeneration_ 的分工：后者闭合「读库 → 提交轨迹」窗口
    //（trackFaces 内比对），前者闭合「拷贝 → 绘制 → 发布」窗口（renderResults 内比对）。
    // 单调递增，仅在外部撤销身份时变化。
    std::atomic<uint64_t> identityVersion_{0};

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
