#include "rk_win/FramePipeline.h"
#include "rk_win/RuntimeBootstrap.h"

#include "rk_win/DnnSsdFaceDetector.h"
#include "rk_win/OverlayRenderer.h"
#include "rk_win/EventLogger.h"
#include "rk_win/FrameProcessor.h"
#include "rk_win/SideEffectSink.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace rk_win {
namespace {
thread_local const FramePipeline* publishingPipeline = nullptr;
struct PublicationScope {
    const FramePipeline* previous;
    explicit PublicationScope(const FramePipeline* pipeline) : previous(publishingPipeline) { publishingPipeline = pipeline; }
    ~PublicationScope() { publishingPipeline = previous; }
};
}

// 不可变运行时快照：recognizer/dnn/processor 共享所有权。
// processLoop 与 applyCameraSettings 通过快照持有当前 runtime 整个调用期间，
// reloadRuntime 原子替换快照，已持快照者不受影响（并发 reload/enroll 无 UAF）。
struct RuntimeSnapshot {
    std::shared_ptr<IRecognizer> recognizer;
    std::shared_ptr<DnnSsdFaceDetector> dnn;
    std::shared_ptr<FrameProcessor> processor;
};

std::shared_ptr<RuntimeSnapshot> FramePipeline::runtimeSnapshot() const {
    std::lock_guard<std::mutex> lock(runtimeMu_);
    return runtime_;
}

// 运行时是否已装配（见头文件注释）：判据是 runtime_ 快照是否已建立，而非 running_。
bool FramePipeline::runtimeReady() const {
    std::lock_guard<std::mutex> lock(runtimeMu_);
    return runtime_ != nullptr;
}

void applyFlip(cv::Mat& bgr, bool flipX, bool flipY) {
    if (bgr.empty()) return;
    if (!flipX && !flipY) return;
    int code = 1;
    if (flipX && flipY) code = -1;
    else if (flipY) code = 0;
    else code = 1;
    cv::flip(bgr, bgr, code);
}

FramePipeline::FramePipeline() = default;

FramePipeline::~FramePipeline() {
    shutdown();
}

bool FramePipeline::initialize(const AppConfig& cfg) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMu_);
    // 活跃线程持有相机与运行时；重复初始化必须在任何资源突变前拒绝，避免覆盖 joinable 线程。
    if (running_ || captureThread_.joinable() || processThread_.joinable()) return false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_ = cfg;
    }
    camera_ = std::make_unique<MfCamera>();
    devices_ = MfCamera::enumerateDevices();

    // 日志初始化（保留）
    if (!logger_.open(cfg.log.logDir, cfg.log.maxFileBytes, cfg.log.maxRollFiles)) {
        std::lock_guard<std::mutex> lock(renderMu_);
        render_.status = "日志打开失败（将继续运行但不会落盘日志）";
    }

    // 装配运行时（委托 RuntimeBootstrap）——替代原有的内联构造
    auto bootstrap = RuntimeBootstrap::build(cfg);
    {
        std::lock_guard<std::mutex> lock(modelsMu_);
        activeModels_ = std::move(bootstrap.models);
    }
    auto snap = std::make_shared<RuntimeSnapshot>();
    snap->recognizer = std::move(bootstrap.recognizer);
    snap->dnn = std::move(bootstrap.detector);
    snap->processor = std::make_shared<FrameProcessor>(snap->recognizer.get());
    {
        std::lock_guard<std::mutex> lock(runtimeMu_);
        runtime_ = std::move(snap);
    }

    // 装配失败的处理：**完整保留**原有的 render_.status 写入（不删、不改 —— 既有显示路径靠它），
    // 在其之后**追加**两件事：
    //  ① 一等化为 degraded_（供 /api/v1/health 只读发布；本批前该告警从不上线）；
    //  ② 落一条 model_init_failed 事件到 EventLogger（落盘可见性）。
    // ⚠️ 两件新增都不改阻断语义：本函数下方仍恒 return true，调用方（两个 main）也仍不检查返回值。
    if (!bootstrap.ok) {
        std::lock_guard<std::mutex> lock(renderMu_);
        render_.status = bootstrap.warning;
    }
    updateDegradedState(bootstrap.ok, bootstrap.warning);
    {
        // 事件落盘：events_ 由 eventsMu_ 保护，EventLogger::append 自身另取自己的 mu_，
        // 不回调 FramePipeline ⇒ 无锁环。必须在 renderMu_ 临界区**之外**（避免锁嵌套）。
        // ⚠️ 落盘可见性**只对本地服务成立**（如实登记，本批不改行为）：本 append 以 events_ 非空为前提，
        //   而在相机 UI 应用里 `setEventLogger` 于 **WM_CREATE**（win_camera_face_recognition_main.cpp:854）
        //   才执行，晚于 wWinMain 的 `app.pipe.initialize(app.cfg)`（同文件 :1227，且在 CreateWindowExW
        //   :1238 **之前**）⇒ 该应用 initialize 时 events_ == nullptr，本 append 是**空操作**
        //   （model_init_failed 不落盘）。要修需调整该应用的初始化顺序（把 setEventLogger 提到
        //   initialize 之前），该文件不在本批文件清单内。
        //   ⚠️ 但**不可**把这条推广到下方 reloadRuntime 侧的同类 append：相机 UI 应用在 WM_CREATE 里
        //   （setEventLogger **之后**，同文件 :856）启动同一个 HttpFacesServer，其
        //   `POST /api/v1/models/reload` 端点会调 `pipe->reloadRuntime(...)`
        //   （JsonEndpointHandlers.cpp:184）⇒ 该应用下**那一处** append 是**可达**的（http.enable 默认
        //   true）。结论：initialize 的 model_init_failed 只在 win_local_service 落盘；reloadRuntime 的
        //   两个调用方（win_local_service_main.cpp:189 与上述 HTTP 端点）都能落盘。两处不可一概而论。
        std::lock_guard<std::mutex> lock(eventsMu_);
        if (events_ && !bootstrap.ok) events_->append("model_init_failed", bootstrap.warning);
    }

    // Manifest check（保留——这是运行时自检，不归 bootstrap 管）
    {
        std::lock_guard<std::mutex> lock(modelsMu_);
        checkModelManifestsLocked();
    }

    // 装配 SideEffectSink（纯计算 + 副作用收口）；FrameProcessor 已在 runtime 快照内
    sink_ = std::make_unique<SideEffectSink>(&logger_, &render_);

    running_ = true;
    processThread_ = std::thread(&FramePipeline::processLoop, this);
    return true;
}

void FramePipeline::shutdown() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMu_);
    running_ = false;
    requestStopCamera();
    frameCv_.notify_all();

    if (captureThread_.joinable()) captureThread_.join();
    if (processThread_.joinable()) processThread_.join();
    logger_.close();
}

std::vector<CameraDevice> FramePipeline::devices() const {
    std::lock_guard<std::mutex> lock(mu_);
    return devices_;
}

std::vector<CameraFormat> FramePipeline::formatsForDeviceIndex(int index) const {
    std::lock_guard<std::mutex> lock(mu_);
    return devices_[static_cast<size_t>(index)].formats;
}

bool FramePipeline::startCameraByIndex(int deviceIndex, int formatIndex) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMu_);
    if (deviceIndex < 0) return false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (deviceIndex >= static_cast<int>(devices_.size())) return false;
        activeDeviceIndex_ = deviceIndex;
        activeFormatIndex_ = formatIndex;
        if (!devices_[static_cast<size_t>(deviceIndex)].formats.empty() && formatIndex >= 0 &&
            formatIndex < static_cast<int>(devices_[static_cast<size_t>(deviceIndex)].formats.size())) {
            desiredFormat_ = devices_[static_cast<size_t>(deviceIndex)].formats[static_cast<size_t>(formatIndex)];
            desiredFormat_.width = cfg_.camera.width;
            desiredFormat_.height = cfg_.camera.height;
            desiredFormat_.fps = cfg_.camera.fps;
        }
        activeCameraNameUtf8_ = utf8FromWide(devices_[static_cast<size_t>(deviceIndex)].name);
        activeCameraIdUtf8_ = utf8FromWide(devices_[static_cast<size_t>(deviceIndex)].deviceId);
    }

    requestStopCamera();
    if (captureThread_.joinable()) captureThread_.join();
    camera_->close();

    {
        std::lock_guard<std::mutex> lock(cameraSignalMu_);
        cameraSignal_ = 0;
    }

    cameraRunning_ = true;
    captureThread_ = std::thread(&FramePipeline::captureLoop, this);
    return true;
}

std::optional<int> FramePipeline::findExactFormatIndexLocked(int deviceIndex, int width, int height, int fps) const {
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(devices_.size())) return std::nullopt;
    const auto& fmts = devices_[static_cast<size_t>(deviceIndex)].formats;
    for (size_t i = 0; i < fmts.size(); i++) {
        const auto& f = fmts[i];
        if (f.width == width && f.height == height && f.fps == fps) return static_cast<int>(i);
    }
    return std::nullopt;
}

CameraSwitchResult FramePipeline::startCameraWithRollbackLocked(int deviceIndex, const CameraFormat& desired, int maxTotalMs) {
    using clock = std::chrono::steady_clock;
    CameraSwitchResult out;
    const auto t0 = clock::now();

    int desiredFormatIndex = -1;
    int prevDeviceIndex = -1;
    int prevFormatIndex = -1;
    std::string prevName;
    std::string prevId;
    CameraFormat prevDesired{};

    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto idxOpt = findExactFormatIndexLocked(deviceIndex, desired.width, desired.height, desired.fps);
        if (!idxOpt) {
            out.ok = false;
            std::ostringstream oss;
            oss << "设备不支持 " << desired.width << "x" << desired.height << " @" << desired.fps << "fps";
            if (deviceIndex >= 0 && deviceIndex < static_cast<int>(devices_.size())) {
                const auto& fmts = devices_[static_cast<size_t>(deviceIndex)].formats;
                std::vector<int> fpsList;
                for (const auto& f : fmts) {
                    if (f.width == desired.width && f.height == desired.height && f.fps > 0) fpsList.push_back(f.fps);
                }
                std::sort(fpsList.begin(), fpsList.end());
                fpsList.erase(std::unique(fpsList.begin(), fpsList.end()), fpsList.end());
                if (!fpsList.empty()) {
                    oss << "；该分辨率可用FPS=";
                    for (size_t i = 0; i < fpsList.size(); i++) {
                        if (i) oss << "/";
                        oss << fpsList[i];
                    }
                    oss << "；请在日志中查看设备枚举到的格式列表";
                }
            }
            out.reason = oss.str();
            return out;
        }
        desiredFormatIndex = *idxOpt;
        prevDeviceIndex = activeDeviceIndex_;
        prevFormatIndex = activeFormatIndex_;
        prevDesired = desiredFormat_;
        prevName = activeCameraNameUtf8_;
        prevId = activeCameraIdUtf8_;

        activeDeviceIndex_ = deviceIndex;
        activeFormatIndex_ = desiredFormatIndex;
        desiredFormat_ = desired;
        activeCameraNameUtf8_ = utf8FromWide(devices_[static_cast<size_t>(deviceIndex)].name);
        activeCameraIdUtf8_ = utf8FromWide(devices_[static_cast<size_t>(deviceIndex)].deviceId);
    }

    requestStopCamera();
    if (captureThread_.joinable()) captureThread_.join();
    camera_->close();

    {
        std::lock_guard<std::mutex> lock(cameraSignalMu_);
        cameraSignal_ = 0;
    }

    cameraRunning_ = true;
    captureThread_ = std::thread(&FramePipeline::captureLoop, this);

    bool gotFirstFrame = false;
    bool openFailed = false;
    CameraOpenResult lastOpen{};
    {
        // 真正等待首帧（captureLoop 异步 open+采集 → cameraSignal_==4 成功 / ==3 打开失败），
        // 超时 maxTotalMs 视为切换失败。此前仅检查一次（cameraSignal_ 刚重置为 0）恒判失败。
        std::unique_lock<std::mutex> lock(cameraSignalMu_);
        cameraSignalCv_.wait_for(lock, std::chrono::milliseconds(maxTotalMs),
                                 [&] { return cameraSignal_ == 4 || cameraSignal_ == 3; });
        if (cameraSignal_ == 4) gotFirstFrame = true;
        else if (cameraSignal_ == 3) openFailed = true;
        lastOpen = lastCameraOpenResult_;
    }

    const auto t1 = clock::now();
    const auto cameraMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    out.width = desired.width;
    out.height = desired.height;
    out.fps = desired.fps;
    out.cameraMs = cameraMs;

    if (!gotFirstFrame) {
        out.ok = false;
        if (openFailed) out.reason = "摄像头打开失败: " + lastOpen.code + " " + lastOpen.message;
        else out.reason = "切换超时：未在目标时间内收到首帧";

        bool rolledBack = false;
        if (prevDeviceIndex >= 0) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                activeDeviceIndex_ = prevDeviceIndex;
                activeFormatIndex_ = prevFormatIndex;
                desiredFormat_ = prevDesired;
                activeCameraNameUtf8_ = prevName;
                activeCameraIdUtf8_ = prevId;
            }

            requestStopCamera();
            if (captureThread_.joinable()) captureThread_.join();
            camera_->close();

            {
                std::lock_guard<std::mutex> lock(cameraSignalMu_);
                cameraSignal_ = 0;
            }

            cameraRunning_ = true;
            captureThread_ = std::thread(&FramePipeline::captureLoop, this);
            rolledBack = true;
        }
        out.rolledBack = rolledBack;
        out.totalMs = cameraMs;
        return out;
    }

    const auto td0 = clock::now();
    auto rt = runtimeSnapshot();
    if (rt && rt->dnn && rt->dnn->ready()) {
        // dnnMu_ 已确认无共享不变式可护（A3）：快照保活 dnn 共享所有权，resetForStream 现为空操作；
        // 删除无用锁不改变线程安全语义（B06-014）
        rt->dnn->resetForStream();
    }
    const auto td1 = clock::now();
    out.detectorMs = std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count();

    const auto t2 = clock::now();
    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t0).count();
    out.totalMs = totalMs;
    out.ok = (totalMs <= maxTotalMs);
    if (!out.ok) {
        out.reason = "切换耗时超标（已按口径判定失败） total_ms=" + std::to_string(totalMs);
        bool rolledBack = false;
        if (prevDeviceIndex >= 0) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                activeDeviceIndex_ = prevDeviceIndex;
                activeFormatIndex_ = prevFormatIndex;
                desiredFormat_ = prevDesired;
                activeCameraNameUtf8_ = prevName;
                activeCameraIdUtf8_ = prevId;
            }

            requestStopCamera();
            if (captureThread_.joinable()) captureThread_.join();
            camera_->close();

            {
                std::lock_guard<std::mutex> lock(cameraSignalMu_);
                cameraSignal_ = 0;
            }

            cameraRunning_ = true;
            captureThread_ = std::thread(&FramePipeline::captureLoop, this);
            rolledBack = true;
        }
        out.rolledBack = rolledBack;
        return out;
    }

    return out;
}

CameraSwitchResult FramePipeline::applyCameraSettings(int deviceIndex, int width, int height, int fps, int maxTotalMs) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMu_);
    CameraFormat desired;
    desired.width = width;
    desired.height = height;
    desired.fps = fps;
    return startCameraWithRollbackLocked(deviceIndex, desired, maxTotalMs);
}

void FramePipeline::requestStopCamera() {
    cameraRunning_ = false;
}

void FramePipeline::stopCameraLocked() {
    cameraRunning_ = false;
    if (captureThread_.joinable()) captureThread_.join();
    camera_->close();
}

CameraOpenResult FramePipeline::openOnce() {
    CameraDevice dev;
    CameraFormat fmt;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (activeDeviceIndex_ < 0 || static_cast<std::size_t>(activeDeviceIndex_) >= devices_.size()) {
            CameraOpenResult r;
            r.ok = false;
            r.category = ErrorCategory::BackendFailure;
            r.code = "no_device";
            r.message = "没有活动摄像头设备";
            return r;
        }
        dev = devices_[static_cast<std::size_t>(activeDeviceIndex_)];
        fmt = desiredFormat_;
    }
    return camera_->open(dev.deviceId, fmt.width, fmt.height, fmt.fps);
}

void FramePipeline::setFlip(bool flipX, bool flipY) {
    std::lock_guard<std::mutex> lock(mu_);
    flipX_ = flipX;
    flipY_ = flipY;
}

void FramePipeline::setEventLogger(EventLogger* logger) {
    // ⚠️ 如实口径：本函数在上一批之前是**事实上的 no-op** —— FramePipeline 内的 events_ 只被写入、
    // 从未被读取，因为 FramePipeline 内当时**不存在**任何 events_->append(...) 调用点。本批新增的
    // model_init_failed 事件（initialize / reloadRuntime）是 FramePipeline 侧**首个**真实消费者，
    // 也就是说：设置事件日志器的接线自本批起才真正生效。不要据旧注释以为它早已有消费者。
    // ⚠️ 原注释在此断言「全仓核实：append 调用点仅在 AbcTestRunner / HttpFacesPoster 各自的同名成员上」
    //   ——该「全仓核实」句**不实**，已在本次修复中订正（结论不变：这句话只对 **FramePipeline 内部**
    //   成立；上面那段「此前无读取方」的结论仍然正确）。事实是：调用方传进来的**同一个** EventLogger
    //   对象在别处另有多处写入点 ——
    //   · win_local_service_main.cpp：7 处 `events.append(...)`（:59 / :64 / :66 / :68 / :127 / :129 / :213），
    //     对象由 :132 的 `pipe.setEventLogger(&events)` 传入；
    //   · win_camera_face_recognition_main.cpp：6 处 `g_app->events.append(...)`（:690 / :711 / :715 /
    //     :718 / :742 / :762），对象由 :854 的 `g_app->pipe.setEventLogger(&g_app->events)` 传入
    //     （注意同文件 :1118 的 `g_app->metrics.append(...)` 的类型是 **RenderMetricsLogger**
    //     ——AppState 成员 `RenderMetricsLogger metrics`（win_camera_face_recognition_main.cpp 的
    //     `#include "rk_win/RenderMetricsLogger.h"` 与 `struct AppState` 处），落盘文件是
    //     `render_metrics.jsonl`（见 RenderMetricsLogger.cpp 的 open）——**不是**本函数的 EventLogger
    //     对象。⚠️ 旧注释此处写「属 StructuredLogger」，**类型说错了**，本次订正；「:1118 不是同一个
    //     EventLogger 对象」这一**排除结论不变**，只是理由从「属 StructuredLogger」改为「属
    //     RenderMetricsLogger」。误判成因：RenderMetricsLogger.cpp 会 include StructuredLogger.h
    //     （借其转义辅助），据 include 关系**反推**类型正是这次出错的路径）；
    //   · 另有 AbcTestRunner / HttpFacesPoster 各自持有的同名成员（旧注释唯一说对的部分）。
    std::lock_guard<std::mutex> lock(eventsMu_);
    events_ = logger;
}

void FramePipeline::setPreviewLayout(int previewW, int previewH, int previewScaleMode) {
    std::lock_guard<std::mutex> lock(previewMu_);
    previewW_ = previewW;
    previewH_ = previewH;
    previewScaleMode_ = previewScaleMode;
}

// ── ReloadPolicy 热更新方法 ──

void FramePipeline::applyModelAndCameraChanges(const AppConfig& previous, const AppConfig& next) {
    const bool cameraChanged = previous.camera.preferredDeviceId != next.camera.preferredDeviceId ||
        previous.camera.width != next.camera.width || previous.camera.height != next.camera.height ||
        previous.camera.fps != next.camera.fps;
    const bool modelChanged = previous.model.faceProfileDir != next.model.faceProfileDir ||
        previous.model.recognition != next.model.recognition || previous.dnn.enable != next.dnn.enable ||
        previous.dnn.modelPath != next.dnn.modelPath || previous.recognition.cascadePath != next.recognition.cascadePath ||
        previous.recognition.databasePath != next.recognition.databasePath;
    // 一域失败不吞掉另一域请求；两个方法各自负责保留对应的旧可用状态。
    if (modelChanged) reloadRuntime(next);
    if (cameraChanged) switchCamera(next);
}

void FramePipeline::switchCamera(const AppConfig& cfg) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMu_);
    // 从 preferredDeviceId 解析设备索引（与 ensureCameraRunning 逻辑一致）
    int idx = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!cfg.camera.preferredDeviceId.empty()) {
            for (int i = 0; i < static_cast<int>(devices_.size()); i++) {
                if (devices_[static_cast<size_t>(i)].deviceId == cfg.camera.preferredDeviceId) {
                    idx = i;
                    break;
                }
            }
        }
    }

    CameraFormat desired;
    desired.width = cfg.camera.width;
    desired.height = cfg.camera.height;
    desired.fps = cfg.camera.fps;

    // 真正原子替换活动采集源（重启 captureLoop + 失败回滚），
    // 避免仅探测却未真正切换采集管线的问题
    const auto result = startCameraWithRollbackLocked(idx, desired, 300);
    {
        std::lock_guard<std::mutex> lock(renderMu_);
        render_.status = result.rolledBack ? "切换失败已回滚" : (result.ok ? "摄像头已切换" : result.reason);
    }
}

void FramePipeline::setCameraForTest(std::unique_ptr<ICameraSource> cam) {
    camera_ = std::move(cam);
}

void FramePipeline::setDevicesForTest(std::vector<CameraDevice> devs) {
    std::lock_guard<std::mutex> lock(mu_);
    devices_ = std::move(devs);
}

void FramePipeline::setRecognizerForTest(std::shared_ptr<IRecognizer> rec) {
    // 原子替换快照内 recognizer 并成对重建 processor（旧 processor 持有旧 recognizer 裸指针，
    // 必须一起换避免新旧混用）；仅测试使用，调用时机须在 processLoop 空闲时（见头文件注释）。
    std::lock_guard<std::mutex> lock(runtimeMu_);
    if (!runtime_) return;
    runtime_->recognizer = std::move(rec);
    runtime_->processor = std::make_shared<FrameProcessor>(runtime_->recognizer.get());
}

int FramePipeline::enrollRemainingForTest() const {
    std::lock_guard<std::mutex> lock(mu_);
    return enrollRemaining_;
}

// Manifest 自检：打印每个活动模型的 MODEL_MANIFEST_CHECK 诊断行（纯诊断、不阻塞）。
// 期望值须与 app/src/main/assets/lbpcascade_frontalface.xml 的 git blob（LF 规范）SHA256 同步。
// .gitattributes 已将 assets 声明 binary（不做行尾转换），checkout 后字节与 blob 一致，
// 故部署的 cascade 计算出的运行时 hash 恒等于此值（Q4-2 平台锚点，RW2/R2-3 双审 P2）。
// 更新模型文件后须更新此值。抽出为复用函数（Q4-4）：initialize 与 reloadRuntime 成功替换
// activeModels_ 后均调用，使日志反映重载后的 hash/manifest_match。调用方须已持有 modelsMu_ 锁。
void FramePipeline::checkModelManifestsLocked() const {
    for (const auto& m : activeModels_) {
        const char* match = "unknown";
        if (m.id == "cascade_frontalface") {
            match = (m.hash == "529f217132809f287aaed5cd35dc00d9bc9b2afebe46dd1fe90ecb67f1daad0d")
                ? "match" : "mismatch";
        }
        std::fprintf(stderr, "MODEL_MANIFEST_CHECK [id=%s] hash=%s manifest_match=%s\n",
            m.id.c_str(), m.hash.c_str(), match);
    }
}

// 降级状态一等化（FACE-ARCWIN-003 可观测性部分）：把 RuntimeBootstrap 的装配结果转成
// degraded_，供 /api/v1/health 只读发布。纯记账，不改任何阻断语义（initialize 仍恒返回 true）。
// 锁序：degradedMu_ → modelsMu_（调用方不得持 renderMu_ / modelsMu_ 进入，见头文件声明）。
void FramePipeline::updateDegradedState(bool bootstrapOk, const std::string& bootstrapWarning) {
    std::lock_guard<std::mutex> lock(degradedMu_);

    // availableFeatures 两种状态下都刷新：正常态即「可用能力清单」，降级态即「幸存的模型」。
    // 取值必须来自 activeModels_ —— initialize/reloadRuntime 在调用本函数前已把
    // BootstrapResult::models **移入** activeModels_，此时读 bootstrap.models 只会拿到空 vector。
    std::vector<std::string> loaded;
    {
        std::lock_guard<std::mutex> modelsLock(modelsMu_);
        for (const auto& m : activeModels_) {
            if (m.status == "loaded") loaded.push_back(m.id);
        }
    }
    degraded_.availableFeatures = std::move(loaded);

    if (bootstrapOk) {
        // 装配成功 ⇒ 清除降级。degradedSinceIso 一并清空，使其在**下次**降级时重新计时；
        // 若残留上一轮时间，会把「本轮已降级多久」报成跨轮累计，误导排查。
        degraded_.active = false;
        degraded_.reasonCode.clear();
        degraded_.warning.clear();
        degraded_.degradedSinceIso.clear();
        return;
    }

    const bool firstEntry = !degraded_.active;
    degraded_.active = true;
    // 契约码固定为 runtime_init_failed：RuntimeBootstrap 只回传「识别器初始化失败」这一事实，
    // 不区分 cascade 文件 / 识别模型（含节点名）/ database 路径，故不臆造 missing_model 等更细的码。
    degraded_.reasonCode = "runtime_init_failed";
    degraded_.warning = bootstrapWarning;
    // degradedSince 语义是「**首次**进入降级」：连续多次失败不刷新，使监控能算持续时长。
    if (firstEntry) degraded_.degradedSinceIso = nowIso8601Local();
}

std::string FramePipeline::runtimeReloadWarning() const {
    std::lock_guard<std::mutex> lock(runtimeMu_);
    return runtimeReloadWarning_;
}

bool FramePipeline::reloadRuntime(const AppConfig& cfg) {
    // 发布回调仍可只读回查；同步重载会形成 publication → lifecycle 的反向锁序，必须提前拒绝。
    if (publishingPipeline == this) return false;
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMu_);
    // 候选装配会对同一 templateSpaceId 的加密库做整库 load；在途登记/清库帧的 run() 里同样
    // 会经 recognizer->saveDb() 整库落盘。二者必须互斥：否则候选读到旧快照，提交后新库的
    // 整库保存会把在途登记的人员覆盖掉（清库则可能被候选快照重新写回而复活人员）。
    // 锁序：lifecycleMu_ → databaseMutationMu_ → publicationMu_（见头文件字段注释）。
    std::unique_lock<std::mutex> databaseLock(databaseMutationMu_);
    auto bootstrap = RuntimeBootstrap::build(cfg);
    if (!bootstrap.ok) {
        // 候选装配失败不能丢弃仍可工作的配置、快照、模型清单与降级状态。
        bool initialFailure = false;
        {
            std::scoped_lock lock(mu_, runtimeMu_, modelsMu_);
            if (!runtime_) {
                // 初次装配没有可保留的旧运行时，须与 initialize 一样发布降级诊断，不能伪装仍在启动。
                auto failed = std::make_shared<RuntimeSnapshot>();
                failed->recognizer = std::move(bootstrap.recognizer);
                failed->dnn = std::move(bootstrap.detector);
                failed->processor = std::make_shared<FrameProcessor>(failed->recognizer.get());
                runtime_ = std::move(failed);
                cfg_ = cfg;
                activeModels_ = std::move(bootstrap.models);
                initialFailure = true;
            }
            runtimeReloadWarning_ = bootstrap.warning;
        }
        if (initialFailure) updateDegradedState(false, bootstrap.warning);
        {
            std::lock_guard<std::mutex> lock(renderMu_);
            render_.status = bootstrap.warning;
        }
        {
            std::lock_guard<std::mutex> lock(eventsMu_);
            if (events_) events_->append("model_init_failed", bootstrap.warning);
        }
        runtimeReloadGen_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto snap = std::make_shared<RuntimeSnapshot>();
    snap->recognizer = std::move(bootstrap.recognizer);
    snap->dnn = std::move(bootstrap.detector);
    snap->processor = std::make_shared<FrameProcessor>(snap->recognizer.get());
    // 装配在屏障外进行，旧预览可继续；提交与整帧发布互斥，不能在检查后释放再发布。
    std::lock_guard<std::mutex> publicationLock(publicationMu_);
    bool cancelledEnrollment = false;
    {
        // 配置读者使用 mu_；同一次成功提交发布配套的配置、运行时和模型快照。
        std::scoped_lock lock(mu_, runtimeMu_, modelsMu_);
        cfg_ = cfg;
        runtime_ = std::move(snap);
        // 登记与清库只作用于请求时的运行时；切换后必须重新发起，避免写入另一模型空间。
        ++enrollEpoch_;
        cancelledEnrollment = enrollRequested_.load();
        enrollRequested_ = false;
        enrollRemaining_ = 0;
        clearDbRequested_ = false;
        runtimeReloadWarning_.clear();
        activeModels_ = std::move(bootstrap.models);
        // Q4-4：重载成功后重跑 manifest 校验——activeModels_ 已替换为 reload 后的模型，
        // 校验随之重跑，使日志反映重载后的 hash/manifest_match（与 initialize 一致，纯诊断不阻塞）。
        checkModelManifestsLocked();
    }
    {
        std::lock_guard<std::mutex> lock(renderMu_);
        // 旧画面已绘制身份标签，必须连同结构化身份清空，避免新空间沿用旧叠加像素。
        render_.faces.clear();
        render_.bgr.release();
        render_.status = cancelledEnrollment ? "模型已切换，登记已取消" : "模型运行时已重载";
    }
    updateDegradedState(true, bootstrap.warning);
    // 重载完成，代次 +1：成功重载必然改变代次（B06-020 治理项，禁止"伪成功"——
    // reload 端点据此向调用方证明重载确实发生，而非仅返回占位状态）。
    runtimeReloadGen_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void FramePipeline::updatePreviewLayout(int w, int h, const std::string& scaleMode) {
    // 只更新预览布局，不重建任何线程
    std::lock_guard<std::mutex> lock(previewMu_);
    previewW_ = w;
    previewH_ = h;
    previewScaleMode_ = std::stoi(scaleMode);
}

void FramePipeline::requestEnroll(const std::string& personId) {
    if (personId.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    enrollPersonId_ = personId;
    enrollRemaining_ = cfg_.recognition.enrollSamples;
    // 新注册递增代次：处理中的旧命令回写时凭 epoch 差异识别"期间有新注册"，
    // 避免旧命令的递减/完成结果覆盖新请求的计数与 pending 标志（B06-018 残余竞态修复）。
    ++enrollEpoch_;
    enrollRequested_ = true;
}

void FramePipeline::requestClearDb() {
    // 与运行时提交排序，不能在切换的作废窗口中把旧清库请求带给新库。
    std::lock_guard<std::mutex> lock(mu_);
    clearDbRequested_ = true;
}

void FramePipeline::injectFrameForTest(const cv::Mat& bgr) {
    std::lock_guard<std::mutex> lock(frameMu_);
    latestFrame_ = bgr.clone();
    latestFrameTs_ = 0;
    hasFrame_ = true;
    frameCv_.notify_one();
}

bool FramePipeline::tryGetRenderState(RenderState& out) {
    std::lock_guard<std::mutex> lock(renderMu_);
    if (render_.bgr.empty()) return false;
    render_.bgr.copyTo(out.bgr);
    out.faces = render_.faces;
    out.fps = render_.fps;
    out.inferMs = render_.inferMs;
    out.dropRate = render_.dropRate;
    out.stride = render_.stride;
    out.timestamp100ns = render_.timestamp100ns;
    out.status = render_.status;
    return true;
}

bool FramePipeline::snapshotFaces(FacesSnapshot& out) {
    std::lock_guard<std::mutex> lock(renderMu_);
    if (render_.bgr.empty()) return false;
    out.faces = render_.faces;
    out.frameWidth = render_.bgr.cols;
    out.frameHeight = render_.bgr.rows;
    out.timestamp100ns = render_.timestamp100ns;
    out.inferMs = render_.inferMs;
    out.dropRate = render_.dropRate;
    out.stride = render_.stride;
    {
        std::lock_guard<std::mutex> lock(previewMu_);
        out.previewWidth = previewW_;
        out.previewHeight = previewH_;
        out.previewScaleMode = previewScaleMode_;
    }
    return true;
}

std::uint64_t FramePipeline::currentFacesSeq() const {
    std::lock_guard<std::mutex> lock(facesSeqMu_);
    return facesSeq_;
}

bool FramePipeline::waitFacesSeqChanged(std::uint64_t lastSeq, int timeoutMs, std::uint64_t& outSeq) const {
    std::unique_lock<std::mutex> lock(facesSeqMu_);
    facesSeqCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this, lastSeq] {
        return facesSeq_ != lastSeq;
    });
    outSeq = facesSeq_;
    return facesSeq_ != lastSeq;
}

bool FramePipeline::lastErrorIsPrivacyDenied() const {
    return lastPrivacyDenied_.load();
}

void FramePipeline::openPrivacySettings() const {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-webcam", nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void FramePipeline::captureLoop() {
    int deviceIndex = -1;
    bool flipX = false;
    bool flipY = false;
    CameraDevice d;
    CameraFormat f;

    {
        std::lock_guard<std::mutex> lock(mu_);
        deviceIndex = activeDeviceIndex_;
        flipX = flipX_;
        flipY = flipY_;
        d = devices_[static_cast<size_t>(deviceIndex)];
        f = desiredFormat_;
    }

    lastPrivacyDenied_ = false;
    int reopenBudget = 3;
    CameraOpenResult openRes = openOnce();
    while (!openRes.ok && reopenBudget-- > 0 && running_ && cameraRunning_ && openRes.category == ErrorCategory::BackendFailure) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        openRes = openOnce();
    }
    if (!openRes.ok) {
        lastPrivacyDenied_ = (openRes.category == ErrorCategory::PrivacyDenied);
        {
            std::lock_guard<std::mutex> lock(cameraSignalMu_);
            cameraSignal_ = 3;
            lastCameraOpenResult_ = openRes;
        }
        cameraSignalCv_.notify_all();
        FrameLogEntry le;
        le.tsIso8601 = nowIso8601Local();
        {
            std::lock_guard<std::mutex> lock(mu_);
            le.cameraName = activeCameraNameUtf8_;
            le.cameraId = activeCameraIdUtf8_;
        }
        le.errorCategory = openRes.category;
        le.errorCode = openRes.code;
        le.errorMessage = openRes.message;
        logger_.append(le);
        {
            std::lock_guard<std::mutex> lock(renderMu_);
            render_.status = "摄像头打开失败: " + openRes.code + " " + openRes.message;
        }
        cameraRunning_ = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(renderMu_);
        render_.status = "摄像头已打开";
    }
    {
        std::lock_guard<std::mutex> lock(cameraSignalMu_);
        cameraSignal_ = 2;
        lastCameraOpenResult_ = openRes;
    }
    cameraSignalCv_.notify_all();

    int consecutiveFail = 0;
    while (running_ && cameraRunning_) {
        cv::Mat bgr;
        std::uint64_t ts = 0;
        const auto rr = camera_->readFrameBgr(bgr, ts);
        if (!rr.ok) {
            lastPrivacyDenied_ = (rr.category == ErrorCategory::PrivacyDenied);
            {
                std::lock_guard<std::mutex> lock(renderMu_);
                render_.status = "采集失败: " + rr.code + " " + rr.message;
            }
            consecutiveFail++;
            if (consecutiveFail == 1) {
                FrameLogEntry le;
                le.tsIso8601 = nowIso8601Local();
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    le.cameraName = activeCameraNameUtf8_;
                    le.cameraId = activeCameraIdUtf8_;
                }
                le.errorCategory = rr.category;
                le.errorCode = rr.code;
                le.errorMessage = rr.message;
                logger_.append(le);
            }
            if (consecutiveFail >= 10 && rr.category == ErrorCategory::BackendFailure) {
                camera_->close();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                CameraDevice dev;
                CameraFormat fmt;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    if (activeDeviceIndex_ >= 0 && static_cast<size_t>(activeDeviceIndex_) < devices_.size()) {
                        dev = devices_[static_cast<size_t>(activeDeviceIndex_)];
                    }
                    fmt = desiredFormat_;
                }
                const auto re = camera_->open(dev.deviceId, fmt.width, fmt.height, fmt.fps);
                if (!re.ok) {
                    FrameLogEntry le;
                    le.tsIso8601 = nowIso8601Local();
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        le.cameraName = activeCameraNameUtf8_;
                        le.cameraId = activeCameraIdUtf8_;
                    }
                    le.errorCategory = re.category;
                    le.errorCode = re.code;
                    le.errorMessage = std::string("重连失败: ") + re.message;
                    logger_.append(le);
                    cameraRunning_ = false;
                    break;
                }
                consecutiveFail = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        consecutiveFail = 0;
        {
            std::lock_guard<std::mutex> lock(cameraSignalMu_);
            if (cameraSignal_ != 4) cameraSignal_ = 4;
        }
        cameraSignalCv_.notify_all();

        {
            std::lock_guard<std::mutex> lock(mu_);
            flipX = flipX_;
            flipY = flipY_;
        }
        applyFlip(bgr, flipX, flipY);

        {
            std::lock_guard<std::mutex> lock(frameMu_);
            captureFrameCount_++;
            if (hasFrame_) overwriteDropCount_++;
            latestFrame_ = std::move(bgr);
            latestFrameTs_ = ts;
            hasFrame_ = true;
        }
        frameCv_.notify_one();
    }

    camera_->close();
}

void FramePipeline::processLoop() {
    using clock = std::chrono::steady_clock;
    auto lastFpsT = clock::now();
    std::uint64_t frames = 0;
    double fps = 0.0;
    std::uint64_t lastCap = 0;
    std::uint64_t lastDrop = 0;
    double lastDropRate = 0.0;
    cv::Mat frameBuffer;

    while (running_) {
        // ── 帧获取（保持 captureLoop → processLoop 的缓冲传递机制） ──
        std::uint64_t ts = 0;
        {
            std::unique_lock<std::mutex> lock(frameMu_);
            if (!running_) break;
            frameCv_.wait_for(lock, std::chrono::milliseconds(1000),
                              [this] { return hasFrame_ || !running_; });
            if (!hasFrame_ || !running_) continue;
            frameBuffer = std::move(latestFrame_);
            ts = latestFrameTs_;
            hasFrame_ = false;
        }

        // FPS 与采集覆盖丢帧统计；识别仍逐帧执行，stride 固定为 1。
        frames++;
        const auto now = clock::now();
        const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFpsT).count();
        if (dt >= 1000) {
            fps = (dt > 0) ? (1000.0 * static_cast<double>(frames) / static_cast<double>(dt)) : 0.0;
            frames = 0;
            lastFpsT = now;
            const std::uint64_t cap = captureFrameCount_.load();
            const std::uint64_t drop = overwriteDropCount_.load();
            const std::uint64_t capDelta = cap - lastCap;
            const std::uint64_t dropDelta = drop - lastDrop;
            lastCap = cap;
            lastDrop = drop;
            lastDropRate = (capDelta > 0) ? (static_cast<double>(dropDelta) / static_cast<double>(capDelta)) : 0.0;
        }

        // ── 构建控制命令 ──
        ControlCommand cmd;
        std::shared_ptr<RuntimeSnapshot> rt;
        // enroll 快照在 mu_ 锁内原子读取：enrollRequested_/personId/remaining/epoch 必须来自
        // 同一注册代次；若标志在锁外读、字段在锁内读（TOCTOU），新请求可能被旧命令的
        // 回写吃掉，epoch 守卫也会因"命令携带新 epoch + 旧标志"错配而失效。
        {
            // 命令和运行时必须来自同一次提交，不能取完旧命令后搭配新识别器。
            std::scoped_lock lock(mu_, runtimeMu_);
            rt = runtime_;
            cmd.clearDb = clearDbRequested_.exchange(false);
            cmd.enrollRequested = enrollRequested_.load();
            cmd.enrollPersonId = enrollPersonId_;
            cmd.enrollRemaining = enrollRemaining_;
            cmd.enrollEpoch = enrollEpoch_;
        }
        cmd.detectStride = detectStride_;
        cmd.frameCounter = frameIndex_;

        // ── 纯计算（检测/识别/清库/注册） ──
        // 与命令配套的快照保活整个 run()；重载只会作废旧帧的登记计数回写。
        if (!rt) continue;
        // 仅对会写库的命令（登记/清库）取数据库锁：与 reloadRuntime 的候选整库 load 互斥，
        // 保证候选看到的是在途 saveDb 落盘之后的库（不丢在途登记，也不会清库后复活）。
        // 纯识别不取此锁 ⇒ 候选装配期间旧预览继续发布。
        // run() 一结束立即释放，再进 publication 屏障，维持
        // lifecycleMu_ → databaseMutationMu_ → publicationMu_ 的锁序（不可倒置）。
        const bool mutatesDatabase = cmd.clearDb || (cmd.enrollRequested && cmd.enrollRemaining > 0);
        std::unique_lock<std::mutex> databaseLock(databaseMutationMu_, std::defer_lock);
        if (mutatesDatabase) {
            databaseLock.lock();
            // 取锁后复核运行时仍是当前：等待期间可能已成功重载（reload 持此锁完成候选加载
            // 与提交），旧快照不得再把结果写进已切换的空间。
            if (rt != runtimeSnapshot()) continue;
        }
        auto result = rt->processor->run(std::move(frameBuffer), cmd);
        if (databaseLock.owns_lock()) databaseLock.unlock();

        // 成功提交后，旧计算结果不得进入状态、日志或回调；屏障须覆盖下面整段发布。
        std::lock_guard<std::mutex> publicationLock(publicationMu_);
        if (rt != runtimeSnapshot()) continue;
        PublicationScope publicationScope(this);

        // ── 同步已消费命令回成员变量 ──
        detectStride_ = result.consumedCommand.detectStride;
        {
            // 写回 enroll 状态（与 requestEnroll 同锁 mu_，消除跨线程双写数据竞争 B06-018）。
            // epoch 守卫：cmd 携带构建时快照的 enrollEpoch_；回写时若当前 enrollEpoch_ 已递增，
            // 说明 run() 期间有新 requestEnroll 到达——旧命令的递减结果/完成标志不得覆盖新请求
            // 的计数与 pending 标志（修复既有注释承认的残余竞态：旧计数 12→11 覆盖新请求的 12）。
            std::lock_guard<std::mutex> lock(mu_);
            if (result.consumedCommand.enrollEpoch == enrollEpoch_) {
                enrollRemaining_ = result.consumedCommand.enrollRemaining;
                if (!result.consumedCommand.enrollRequested) {
                    enrollRequested_ = false;
                }
            }
        }

        // ── 构建日志条目元数据 ──
        FrameLogEntry le;
        le.tsIso8601 = nowIso8601Local();
        le.frameIndex = frameIndex_++;
        le.fps = fps;
        {
            std::lock_guard<std::mutex> lock(mu_);
            le.cameraName = activeCameraNameUtf8_;
            le.cameraId = activeCameraIdUtf8_;
        }

        // ── 副作用收口 + 渲染态状态文本（bgr/status 同临界区，读侧见原子一致快照） ──
        {
            std::lock_guard<std::mutex> lock(renderMu_);
            sink_->publish(result, le);
            render_.fps = fps;
            render_.inferMs = result.inferMs;
            render_.dropRate = lastDropRate;
            render_.stride = detectStride_;
            render_.timestamp100ns = ts;

            if (result.consumedCommand.clearDb) {
                render_.status = "已清空人脸库";
            }
            if (cmd.enrollRequested) {
                // B06-018/RW3 遗留：epoch 失配时 status 须读当前 pending，避免显示旧注册。
                // 失配表示 run() 期间有新登记或运行时已切换；旧命令的信息不得覆盖当前状态。
                // 当前有 pending 时显示新请求，否则显示模型切换导致的取消。
                // epoch 比较与失配分支读当前 pending 共持一把 mu_ 锁：enrollEpoch_ 与
                // enrollRequested_/enrollPersonId_/enrollRemaining_ 同受 mu_ 保护，原两段相邻
                // 独立锁块合并为一次获取，减少锁开销并消除两次取锁间请求到达的显示不一致。
                bool epochMismatch;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    epochMismatch = (result.consumedCommand.enrollEpoch != enrollEpoch_);
                    if (epochMismatch) {
                        if (enrollRequested_) {
                            render_.status = "注册中: " + enrollPersonId_
                                + " 还需样本=" + std::to_string(enrollRemaining_);
                        } else {
                            // 成功切换已取消旧登记，不能把未完成采样呈现为注册完成。
                            render_.status = "模型已切换，登记已取消";
                        }
                    }
                }
                if (!epochMismatch) {
                    if (result.consumedCommand.enrollRequested) {
                        // 读 result.consumedCommand（本线程局部值）而非成员 enrollRemaining_，
                        // 消除对 mu_ 保护成员的无锁读；无并发时两者值恒等（上一行写回同一值）
                        render_.status = "注册中: " + cmd.enrollPersonId
                            + " 还需样本=" + std::to_string(result.consumedCommand.enrollRemaining);
                    } else {
                        render_.status = "注册完成: " + cmd.enrollPersonId;
                    }
                }
            }
        }

        // 回调可能再入快照读取接口，必须等渲染状态整帧提交并释放 renderMu_ 后执行。
        sink_->notifyFaces(result.matches);

        // ── 序列通知 ──
        {
            std::lock_guard<std::mutex> lock(facesSeqMu_);
            facesSeq_++;
        }
        facesSeqCv_.notify_all();
    }
}
}  // namespace rk_win

