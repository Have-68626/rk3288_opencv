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
    cfg_ = cfg;
    devices_ = MfCamera::enumerateDevices();

    // 日志初始化（保留）
    if (!logger_.open(cfg_.log.logDir, cfg_.log.maxFileBytes, cfg_.log.maxRollFiles)) {
        render_.status = "日志打开失败（将继续运行但不会落盘日志）";
    }

    // 装配运行时（委托 RuntimeBootstrap）——替代原有的内联构造
    auto bootstrap = RuntimeBootstrap::build(cfg_);
    {
        std::lock_guard<std::mutex> lock(modelsMu_);
        activeModels_ = std::move(bootstrap.models);
    }
    recognizer_ = std::move(bootstrap.recognizer);
    dnn_ = std::move(bootstrap.detector);

    if (!bootstrap.ok) {
        render_.status = bootstrap.warning;
    }

    // Manifest check（保留——这是运行时自检，不归 bootstrap 管）
    {
        std::lock_guard<std::mutex> lock(modelsMu_);
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

    // 装配 FrameProcessor + SideEffectSink（纯计算 + 副作用收口）
    processor_ = std::make_unique<FrameProcessor>(dnn_.get(), recognizer_.get());
    sink_ = std::make_unique<SideEffectSink>(&logger_, &render_);

    running_ = true;
    processThread_ = std::thread(&FramePipeline::processLoop, this);
    return true;
}

void FramePipeline::shutdown() {
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
    camera_.close();

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
    camera_.close();

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
        std::unique_lock<std::mutex> lock(cameraSignalMu_);
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
            camera_.close();

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
    if (cfg_.dnn.enable && dnn_ && dnn_->ready()) {
        std::lock_guard<std::mutex> lock(dnnMu_);
        dnn_->resetForStream();
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
            camera_.close();

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
    camera_.close();
}

void FramePipeline::setFlip(bool flipX, bool flipY) {
    std::lock_guard<std::mutex> lock(mu_);
    flipX_ = flipX;
    flipY_ = flipY;
}

void FramePipeline::setEventLogger(EventLogger* logger) {
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

void FramePipeline::switchCamera(const AppConfig& cfg) {
    CameraOpenParams params;
    params.width = cfg.camera.width;
    params.height = cfg.camera.height;
    params.fps = cfg.camera.fps;

    // 从 preferredDeviceId 查找设备索引（与 ensureCameraRunning 逻辑一致）
    {
        std::lock_guard<std::mutex> lock(mu_);
        int idx = 0;
        if (!cfg.camera.preferredDeviceId.empty()) {
            for (int i = 0; i < static_cast<int>(devices_.size()); i++) {
                if (devices_[static_cast<size_t>(i)].deviceId == cfg.camera.preferredDeviceId) {
                    idx = i;
                    break;
                }
            }
        }
        params.deviceIndex = idx;
    }

    auto result = CameraSession::switchWithRollback(params, currentDevice_);
    if (result.ok) {
        currentDevice_ = result.device;
    }
    {
        std::lock_guard<std::mutex> lock(renderMu_);
        render_.status = result.code == "ok" ? "摄像头已切换" : result.message;
    }
}

void FramePipeline::reloadRuntime(const AppConfig& cfg) {
    cfg_ = cfg;
    auto bootstrap = RuntimeBootstrap::build(cfg);
    {
        std::lock_guard<std::mutex> lock(modelsMu_);
        activeModels_ = std::move(bootstrap.models);
    }
    recognizer_ = std::move(bootstrap.recognizer);
    dnn_ = std::move(bootstrap.detector);
    // 重建 FrameProcessor（它持有 dnn_ 和 recognizer_ 的裸指针）
    processor_ = std::make_unique<FrameProcessor>(dnn_.get(), recognizer_.get());
    {
        std::lock_guard<std::mutex> lock(renderMu_);
        render_.status = bootstrap.ok ? "模型运行时已重载" : bootstrap.warning;
    }
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
    enrollRequested_ = true;
}

void FramePipeline::requestClearDb() {
    clearDbRequested_ = true;
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
        render_.status = "摄像头打开失败: " + openRes.code + " " + openRes.message;
        cameraRunning_ = false;
        return;
    }

    {
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
        const auto rr = camera_.readFrameBgr(bgr, ts);
        if (!rr.ok) {
            lastPrivacyDenied_ = (rr.category == ErrorCategory::PrivacyDenied);
            render_.status = "采集失败: " + rr.code + " " + rr.message;
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
                camera_.close();
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
                const auto re = camera_.open(dev.deviceId, fmt.width, fmt.height, fmt.fps);
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

    camera_.close();
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
            latestFrame_.copyTo(frameBuffer);
            ts = latestFrameTs_;
            hasFrame_ = false;
        }

        // ── FPS 统计 + 自适应 stride ──
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
        cmd.clearDb = clearDbRequested_.exchange(false);
        cmd.enrollRequested = enrollRequested_.load();
        if (cmd.enrollRequested) {
            std::lock_guard<std::mutex> lock(mu_);
            cmd.enrollPersonId = enrollPersonId_;
            cmd.enrollRemaining = enrollRemaining_;
        }
        cmd.detectStride = detectStride_;
        cmd.frameCounter = frameIndex_;

        // ── 纯计算（检测/识别/清库/注册/背压跳过） ──
        auto result = processor_->run(frameBuffer, cmd);

        // ── 同步已消费命令回成员变量 ──
        detectStride_ = result.consumedCommand.detectStride;
        if (!result.consumedCommand.enrollRequested) {
            enrollRequested_ = false;
        }
        enrollRemaining_ = result.consumedCommand.enrollRemaining;

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

        // ── 副作用收口（Overlay 绘制 + 渲染态发布 + 结构化日志） ──
        {
            std::lock_guard<std::mutex> lock(renderMu_);
            sink_->publish(result, le);
            render_.fps = fps;
            render_.inferMs = result.inferMs;
            render_.dropRate = lastDropRate;
            render_.stride = detectStride_;
            render_.timestamp100ns = ts;
        }

        // ── 渲染态状态文本（publish 不处理） ──
        if (result.consumedCommand.clearDb) {
            render_.status = "已清空人脸库";
        }
        if (cmd.enrollRequested) {
            if (result.consumedCommand.enrollRequested) {
                render_.status = "注册中: " + cmd.enrollPersonId
                    + " 还需样本=" + std::to_string(enrollRemaining_);
            } else {
                render_.status = "注册完成: " + cmd.enrollPersonId;
            }
        }

        // ── 序列通知 ──
        {
            std::lock_guard<std::mutex> lock(facesSeqMu_);
            facesSeq_++;
        }
        facesSeqCv_.notify_all();
    }
}
}  // namespace rk_win

