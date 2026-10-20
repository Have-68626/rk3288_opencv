#pragma once

#include "rk_win/RuntimeBootstrap.h"
#include "FaceRecognizer.h"  // FaceMatch
#include "IRecognizer.h"
#include "MfCamera.h"
#include "StructuredLogger.h"
#include "WinConfig.h"
#include "rk_win/FrameProcessor.h"
#include "rk_win/SideEffectSink.h"

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
class RuntimeSnapshot;  // 运行时快照：recognizer/dnn/processor 共享所有权

struct RenderState {
    cv::Mat bgr;
    std::vector<FaceMatch> faces;
    double fps = 0.0;
    double inferMs = 0.0;
    double dropRate = 0.0;
    int stride = 1;
    std::uint64_t timestamp100ns = 0;
    // 人类可读的运行状态 / 告警文本（在 FramePipeline.cpp 内多处写入）。
    // ⚠️ 如实口径（本次修复订正；原注释称「该字段**写而不读**——三处 tryGetRenderState 调用点都只
    //    取帧、丢弃本字段，故装配告警**从不上线**」，**不实**）：全仓 tryGetRenderState 共 **4** 处
    //    调用点，其中 3 处确实只取帧、丢弃本字段（JsonEndpointHandlers 的 handlePreviewJpg、
    //    StreamSession 的帧提供器、AbcTestRunner），但**第 4 处会读它**——
    //    win_camera_face_recognition_main.cpp 的 WM_TIMER 渲染分支（约 :1085-1093）里
    //    `if (!rs.status.empty())` 把该文本拼进窗口标题（节流 250 ms）⇒ 本字段承载的装配告警在
    //    **相机 UI 应用**里确实上线（只是不上 Web / health 通道）。
    //    装配失败的正规发布通道另有下方的 DegradedState（经 /api/v1/health 只读发布），二者并列、
    //    不替代；**不要**据旧注释以为本字段没有任何消费者，也不要以为它有统一的对外通道。
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

    // ── ReloadPolicy 热更新（Phase 1） ──
    void switchCamera(const AppConfig& cfg);
    // 同一次配置更新可同时改变两域，分别消费各自差异并沿用各自失败回滚。
    void applyModelAndCameraChanges(const AppConfig& previous, const AppConfig& next);
    bool reloadRuntime(const AppConfig& cfg);
    // 最近一次重载失败告警；失败保留旧运行态，不能用 degraded() 推断此次重载结果。
    std::string runtimeReloadWarning() const;
    void updatePreviewLayout(int w, int h, const std::string& scaleMode);

    void requestEnroll(const std::string& personId);
    void requestClearDb();

    // 测试辅助：注入一帧驱动处理循环（无相机环境验证并发 reload/enroll 路径）
    void injectFrameForTest(const cv::Mat& bgr);

    // 测试辅助：注入假相机与设备列表（验证相机切换后采集源确实改变）
    void setCameraForTest(std::unique_ptr<ICameraSource> cam);
    void setDevicesForTest(std::vector<CameraDevice> devs);

    // 测试辅助：注入假识别器（确定性验证 enroll 竞态）。
    // ⚠️ 仅可在 processLoop 空闲时调用（推荐 initialize 后、注入首帧前）：原子替换
    //   快照内 recognizer 并重建 processor；若在 run() 执行中替换，旧 processor 持有的
    //   recognizer 裸指针会悬空（旧对象销毁 → UAF）。
    void setRecognizerForTest(std::shared_ptr<IRecognizer> rec);

    // 测试辅助：读取 enroll 剩余样本数（epoch 守卫回写不覆盖新计数的判别断言）
    int enrollRemainingForTest() const;

    bool tryGetRenderState(RenderState& out);
    bool snapshotFaces(FacesSnapshot& out);
    std::uint64_t currentFacesSeq() const;
    bool waitFacesSeqChanged(std::uint64_t lastSeq, int timeoutMs, std::uint64_t& outSeq) const;

    bool lastErrorIsPrivacyDenied() const;
    void openPrivacySettings() const;

    std::vector<ModelSnapshot> getActiveModels() const { std::lock_guard<std::mutex> lock(modelsMu_); return activeModels_; }

    // ── 降级状态（FACE-ARCWIN-003 可观测性部分）────────────────────────────
    // 装配失败（RuntimeBootstrap 的 ok==false，即识别器整体初始化失败）在此**一等化**，
    // 供 /api/v1/health 只读发布，使「启动成功但功能缺失」不再只藏在「设置→模型状态」Tab 里。
    // 语义与既有的 RenderState::status（FramePipeline.cpp 内 render_.status）**并列、不替代**：
    // 后者仍按原样写入（既有写入行为**不删**），且**并非无消费者**——相机 UI 应用会把它显示在窗口
    // 标题上（见 RenderState::status 的注释，本次修复订正了「无消费者」的旧说法）；二者互不覆盖、不合并。
    // ⚠️ 本状态是**纯读侧可观测性**：它不改变 FramePipeline::initialize 的返回值
    //    （该函数恒返回 true），也不阻断任何操作。「自检失败是否 fail-closed」属产品决策，
    //    未拍板前不得让本状态参与任何阻断或非 2xx 映射。
    struct DegradedState {
        bool active = false;
        // 契约码，取值域为 src/cpp/include/AccelerationContract.h 的 7 个固定码：
        // ok / build_disabled / unsupported_platform / missing_dependency / missing_model /
        // runtime_init_failed / unsupported_input。
        // 本管线**只产 runtime_init_failed**：RuntimeBootstrap 的返回不区分识别器失败的
        // 具体原因（cascade 文件 / 识别模型（含节点名）/ database 路径之一），故不臆造更细的码。
        std::string reasonCode;
        // 人类可读告警 = RuntimeBootstrap 的 warning 原文（**可能含文件系统路径**）。
        std::string warning;
        // **首次**进入降级的时间。格式与全项目其他时间戳一致：nowIso8601Local() 的
        // `%Y-%m-%dT%H:%M:%S.mmm`（**本地时间、无时区后缀**，非 UTC）。
        std::string degradedSinceIso;
        // 当前 status=="loaded" 的模型 id 集合。**两种状态下都刷新**（正常态即「可用能力清单」）。
        std::vector<std::string> availableFeatures;
    };

    // 只读访问器（degradedMu_ 保护，返回副本）：调用方无须持锁。
    DegradedState degraded() const {
        std::lock_guard<std::mutex> lock(degradedMu_);
        return degraded_;
    }

    // 运行时是否**已装配**（initialize 或 reloadRuntime 之一已执行）。
    // 供 /api/v1/health 区分「尚未装配（starting）」与「装配完成」。判据用 runtime_ 快照是否
    // 已建立，**不用 running_**：running_ 在 shutdown 后为 false，会把「已停止」误报为「启动中」。
    bool runtimeReady() const;

    // 运行时重载代次：reloadRuntime **每次执行**都 +1（`fetch_add` 无条件，见其实现末尾；
    // 装配失败也照样递增）。
    // reload 端点（B06-020 治理项）以"成功重载**必然**改变代次"为契约——禁止"伪成功"
    // （返回成功但从未真正重载）；消费方记录已消费代次即可感知重载确实发生。
    // ⚠️ 代次改变只证明重载执行过，不证明装配成功；本次成败以 reloadRuntime 返回值为准。
    std::uint64_t runtimeReloadGen() const { return runtimeReloadGen_.load(std::memory_order_relaxed); }

private:
#if defined(RK_ENABLE_TEST_SEAMS)
    friend struct FramePipelineTestAccess;
#endif
    void captureLoop();
    void processLoop();
    void stopCameraLocked();
    CameraOpenResult openOnce();
    CameraSwitchResult startCameraWithRollbackLocked(int deviceIndex, const CameraFormat& desired, int maxTotalMs);
    std::optional<int> findExactFormatIndexLocked(int deviceIndex, int width, int height, int fps) const;

    // Manifest 自检（Q4-4）：遍历 activeModels_ 打印 MODEL_MANIFEST_CHECK 诊断日志（stderr）。
    // 纯诊断、不阻塞。initialize 与 reloadRuntime 成功替换模型后均调用，使日志反映重载后的
    // hash/manifest_match。调用方须已持有 modelsMu_ 锁（命名 *Locked）。
    void checkModelManifestsLocked() const;

    // 把装配结果一等化为 degraded_（见公开段 DegradedState 注释）。调用方**不得**持有
    // renderMu_ / modelsMu_ / runtimeMu_ —— 本函数自行取 degradedMu_ 与 modelsMu_
    // （固定锁序 degradedMu_ → modelsMu_），在既有临界区内调用会破坏该锁序。
    // 参数直接来自 RuntimeBootstrap::build 的结果（ok / warning），失败时为降级、成功时清除。
    void updateDegradedState(bool bootstrapOk, const std::string& bootstrapWarning);

    AppConfig cfg_{};
    mutable std::mutex mu_;
    // 串行化线程创建、join 与相机切换；不得持 mu_ 调用，采集/处理线程退出仍需 mu_。
    std::mutex lifecycleMu_;
    // 候选加载与数据库变更互斥；纯识别不取此锁，装配期间仍可发布旧预览。
    std::mutex databaseMutationMu_;
    // 提交顺序为 lifecycleMu_ → databaseMutationMu_ → publicationMu_；回调不得同步重载。
    std::mutex publicationMu_;

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

    // 相机采集源：可注入接口（测试用假相机验证切换），initialize 默认注入 MfCamera
    std::unique_ptr<ICameraSource> camera_;
    StructuredLogger logger_;
    // 不可变运行时快照：reloadRuntime 原子替换，processLoop 取快照持有整个 run()，
    // 避免并发 reload/enroll 时旧 processor/recognizer/dnn 被销毁（UAF）
    std::shared_ptr<RuntimeSnapshot> runtime_;
    mutable std::mutex runtimeMu_;
    std::string runtimeReloadWarning_;

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
    // 注册代次（mu_ 保护）：新请求或运行时成功切换时 +1，并随 cmd 携带；回写前比较
    // 当前值与 cmd 携带值——未变才写回（期间有新请求或运行时已切换时，丢弃旧计数，
    // 修复 B06-018 注释承认的残余竞态）。
    std::uint64_t enrollEpoch_ = 0;

    std::atomic<bool> clearDbRequested_{false};

    std::shared_ptr<RuntimeSnapshot> runtimeSnapshot() const;

    std::unique_ptr<SideEffectSink> sink_;

    std::atomic<bool> lastPrivacyDenied_{false};

    mutable std::mutex modelsMu_;

    std::vector<ModelSnapshot> activeModels_;

    // 运行时重载代次：见 runtimeReloadGen() 注释（B06-020 治理项）。
    std::atomic<std::uint64_t> runtimeReloadGen_{0};

    // 降级状态（见公开段 DegradedState）：独立于 renderMu_ / modelsMu_ 的锁。
    // 锁序固定为 degradedMu_ → modelsMu_（见 updateDegradedState）。
    mutable std::mutex degradedMu_;
    DegradedState degraded_;
};

}  // namespace rk_win

