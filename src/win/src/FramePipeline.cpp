#include "rk_win/FramePipeline.h"

#include "rk_win/DnnSsdFaceDetector.h"
#include "rk_win/OverlayRenderer.h"
#include "rk_win/EventLogger.h"
#include "FileHash.h"

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

std::string utf8FromWide(const std::wstring& ws) {
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
#else
    return std::string(ws.begin(), ws.end());
#endif

void applyFlip(cv::Mat& bgr, bool flipX, bool flipY) {
    if (bgr.empty()) return;
    if (!flipX && !flipY) return;
    int code = 1;
    if (flipX && flipY) code = -1;
    else if (flipY) code = 0;
    else code = 1;
    cv::flip(bgr, bgr, code);

FramePipeline::FramePipeline() = default;

FramePipeline::~FramePipeline() {
    shutdown();

bool FramePipeline::initialize(const AppConfig& cfg) {
    cfg_ = cfg;
    devices_ = MfCamera::enumerateDevices();

    if (!logger_.open(cfg_.log.logDir, cfg_.log.maxFileBytes, cfg_.log.maxRollFiles)) {
        render_.status = "日志打开失败（将继续运行但不会落盘日志）";

    {
        std::lock_guard<std::mutex> lock(modelsMu_);
        activeModels_.clear();

    const std::string cascadeUtf8 = cfg_.recognition.cascadePath.string();
    bool cascadeOk = recognizer_.initialize(cascadeUtf8, cfg_.recognition.databasePath, cfg_.recognition.minFaceSizePx, cfg_.recognition.identifyThreshold);
    if (!cascadeOk) {
        render_.status = "识别模块初始化失败（请检查 cascade_path 与 database_path）";

    {
        ModelSnapshot m;
        m.id = "cascade_frontalface";
        m.displayName = "Cascade Frontal Face (LBP)";
        m.taskType = "detect_recognize_pipeline";
        m.configuredPath = cascadeUtf8;
        m.resolvedPath = cascadeUtf8;
        m.backend = "opencv_cascade";
        m.hash = rk_wcfr::calculateSHA256(cfg_.recognition.cascadePath);
        m.status = cascadeOk ? "loaded" : (std::filesystem::exists(cfg_.recognition.cascadePath) ? "failed" : "missing");
        m.isInUse = !cfg_.dnn.enable;
        if (!cascadeOk) m.lastError = "初始化失败 (cascade_path 或 database_path 有误)";
        std::lock_guard<std::mutex> lock(modelsMu_);
        activeModels_.push_back(m);
        std::fprintf(stderr, "MODEL_REGISTRY_SELF_CHECK [id=%s] path=%s backend=%s status=%s error=%s\n", m.id.c_str(), m.resolvedPath.c_str(), m.backend.c_str(), m.status.c_str(), m.lastError.c_str());
    }

    dnn_ = std::make_unique<DnnSsdFaceDetector>();
    if (cfg_.dnn.enable) {
        DnnSsdConfig dc;
        dc.modelPath = cfg_.dnn.modelPath;
        dc.configPath = cfg_.dnn.configPath;
        dc.inputWidth = cfg_.dnn.inputWidth;
        dc.inputHeight = cfg_.dnn.inputHeight;
        dc.scale = cfg_.dnn.scale;
        dc.meanB = cfg_.dnn.meanB;
        dc.meanG = cfg_.dnn.meanG;
        dc.meanR = cfg_.dnn.meanR;
        dc.swapRB = cfg_.dnn.swapRB;
        dc.confThreshold = cfg_.dnn.confThreshold;
        dc.backend = cfg_.dnn.backend;
        dc.target = cfg_.dnn.target;

        std::string err;

        bool dnnOk = dnn_->initialize(dc, err);

        if (!dnnOk) {

            std::lock_guard<std::mutex> lock(renderMu_);

            render_.status = "DNN 检测初始化失败: " + err;

        }

        ModelSnapshot m;

        m.id = "dnn_face_detector";

        m.displayName = "OpenCV DNN Face Detector";

        m.taskType = "detect";

        m.configuredPath = cfg_.dnn.modelPath.string();
        m.resolvedPath = cfg_.dnn.modelPath.string();
        m.backend = "opencv_dnn";
        m.hash = rk_wcfr::calculateSHA256(cfg_.dnn.modelPath);
        m.status = dnnOk ? "loaded" : (std::filesystem::exists(cfg_.dnn.modelPath) ? "failed" : "missing");
        m.isInUse = true;

        m.lastError = err;

        std::lock_guard<std::mutex> lock(modelsMu_);

        activeModels_.push_back(m);
        std::fprintf(stderr, "MODEL_REGISTRY_SELF_CHECK [id=%s] path=%s backend=%s status=%s error=%s\n", m.id.c_str(), m.resolvedPath.c_str(), m.backend.c_str(), m.status.c_str(), m.lastError.c_str());
    } else {
        ModelSnapshot m;
        m.id = "dnn_face_detector";
        m.displayName = "OpenCV DNN Face Detector";
        m.taskType = "detect";
        m.configuredPath = cfg_.dnn.modelPath.string();
        m.resolvedPath = cfg_.dnn.modelPath.string();
        m.backend = "opencv_dnn";
        m.hash = rk_wcfr::calculateSHA256(cfg_.dnn.modelPath);
        m.status = "disabled";
        m.isInUse = false;
        std::lock_guard<std::mutex> lock(modelsMu_);
        activeModels_.push_back(m);
        std::fprintf(stderr, "MODEL_REGISTRY_SELF_CHECK [id=%s] path=%s backend=%s status=%s error=%s\n", m.id.c_str(), m.resolvedPath.c_str(), m.backend.c_str(), m.status.c_str(), m.lastError.c_str());
    }
    running_ = true;
    processThread_ = std::thread(&FramePipeline::processLoop, this);
    return true;

void FramePipeline::shutdown() {
    running_ = false;
    requestStopCamera();
    frameCv_.notify_all();

    if (captureThread_.joinable()) captureThread_.join();
    if (processThread_.joinable()) processThread_.join();
    logger_.close();

std::vector<CameraDevice> FramePipeline::devices() const {
    std::lock_guard<std::mutex> lock(mu_);
    return devices_;

std::vector<CameraFormat> FramePipeline::formatsForDeviceIndex(int index) const {
    std::lock_guard<std::mutex> lock(mu_);
    return devices_[static_cast<size_t>(index)].formats;

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
        activeCameraNameUtf8_ = utf8FromWide(devices_[static_cast<size_t>(deviceIndex)].name);
        activeCameraIdUtf8_ = utf8FromWide(devices_[static_cast<size_t>(deviceIndex)].deviceId);

    requestStopCamera();
    if (captureThread_.joinable()) captureThread_.join();
    camera_.close();

    {
        std::lock_guard<std::mutex> lock(cameraSignalMu_);
        cameraSignal_ = 0;

    cameraRunning_ = true;
    captureThread_ = std::thread(&FramePipeline::captureLoop, this);
    return true;

std::optional<int> FramePipeline::findExactFormatIndexLocked(int deviceIndex, int width, int height, int fps) const {
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(devices_.size())) return std::nullopt;
    const auto& fmts = devices_[static_cast<size_t>(deviceIndex)].formats;
    for (size_t i = 0; i < fmts.size(); i++) {
        const auto& f = fmts[i];
        if (f.width == width && f.height == height && f.fps == fps) return static_cast<int>(i);
    return std::nullopt;

CameraSwitchResult FramePipeline::startCameraWithRollbackLocked(int deviceIndex, const CameraFormat& desired, int maxTotalMs) {
    using clock = std::chrono::steady_clock;
    CameraSwitchResult out;
    const auto t0 = clock::now();

    int desiredFormatIndex = -1;
    int prevDeviceIndex = -1;
    int prevFormatIndex = -1;
    std::string prevName;
    std::string prevId;

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
                std::sort(fpsList.begin(), fpsList.end());
                fpsList.erase(std::unique(fpsList.begin(), fpsList.end()), fpsList.end());
                if (!fpsList.empty()) {
                    oss << "；该分辨率可用FPS=";
                    for (size_t i = 0; i < fpsList.size(); i++) {
                        if (i) oss << "/";
                        oss << fpsList[i];
                    oss << "；请在日志中查看设备枚举到的格式列表";
            out.reason = oss.str();
            return out;
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

    requestStopCamera();
    if (captureThread_.joinable()) captureThread_.join();
    camera_.close();

    {
        std::lock_guard<std::mutex> lock(cameraSignalMu_);
        cameraSignal_ = 0;

    cameraRunning_ = true;
    captureThread_ = std::thread(&FramePipeline::captureLoop, this);

    bool gotFirstFrame = false;
    bool openFailed = false;
    {
        std::unique_lock<std::mutex> lock(cameraSignalMu_);
        if (cameraSignal_ == 4) gotFirstFrame = true;
        else if (cameraSignal_ == 3) openFailed = true;
        lastOpen = lastCameraOpenResult_;

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

            requestStopCamera();
            if (captureThread_.joinable()) captureThread_.join();
            camera_.close();

            {
                std::lock_guard<std::mutex> lock(cameraSignalMu_);
                cameraSignal_ = 0;

            cameraRunning_ = true;
            captureThread_ = std::thread(&FramePipeline::captureLoop, this);
            rolledBack = true;
        out.rolledBack = rolledBack;
        out.totalMs = cameraMs;
        return out;

    const auto td0 = clock::now();
    if (cfg_.dnn.enable && dnn_ && dnn_->ready()) {
        std::lock_guard<std::mutex> lock(dnnMu_);
        dnn_->resetForStream();
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

            requestStopCamera();
            if (captureThread_.joinable()) captureThread_.join();
            camera_.close();

            {
                std::lock_guard<std::mutex> lock(cameraSignalMu_);
                cameraSignal_ = 0;

            cameraRunning_ = true;
            captureThread_ = std::thread(&FramePipeline::captureLoop, this);
            rolledBack = true;
        out.rolledBack = rolledBack;
        return out;

    return out;

CameraSwitchResult FramePipeline::applyCameraSettings(int deviceIndex, int width, int height, int fps, int maxTotalMs) {
    desired.width = width;
    desired.height = height;
    desired.fps = fps;
    return startCameraWithRollbackLocked(deviceIndex, desired, maxTotalMs);

void FramePipeline::requestStopCamera() {
    cameraRunning_ = false;

void FramePipeline::stopCameraLocked() {
    cameraRunning_ = false;
    if (captureThread_.joinable()) captureThread_.join();
    camera_.close();

void FramePipeline::setFlip(bool flipX, bool flipY) {
    std::lock_guard<std::mutex> lock(mu_);
    flipX_ = flipX;
    flipY_ = flipY;

void FramePipeline::setEventLogger(EventLogger* logger) {
    std::lock_guard<std::mutex> lock(eventsMu_);
    events_ = logger;

void FramePipeline::setPreviewLayout(int previewW, int previewH, int previewScaleMode) {
    std::lock_guard<std::mutex> lock(previewMu_);
    previewW_ = previewW;
    previewH_ = previewH;
    previewScaleMode_ = previewScaleMode;

void FramePipeline::requestEnroll(const std::string& personId) {
    if (personId.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    enrollPersonId_ = personId;
    enrollRemaining_ = cfg_.recognition.enrollSamples;
    enrollRequested_ = true;

void FramePipeline::requestClearDb() {
    clearDbRequested_ = true;

bool FramePipeline::tryGetRenderState(RenderState& out) {
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

bool FramePipeline::snapshotFaces(FacesSnapshot& out) {
    {
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
    return true;

std::uint64_t FramePipeline::currentFacesSeq() const {
    std::lock_guard<std::mutex> lock(facesSeqMu_);
    return facesSeq_;

bool FramePipeline::waitFacesSeqChanged(std::uint64_t lastSeq, int timeoutMs, std::uint64_t& outSeq) const {
    std::unique_lock<std::mutex> lock(facesSeqMu_);
    outSeq = facesSeq_;
    return facesSeq_ != lastSeq;

bool FramePipeline::lastErrorIsPrivacyDenied() const {
    return lastPrivacyDenied_.load();

void FramePipeline::openPrivacySettings() const {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-webcam", nullptr, nullptr, SW_SHOWNORMAL);
#endif

void FramePipeline::captureLoop() {
    int deviceIndex = -1;
    bool flipX = false;
    bool flipY = false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        deviceIndex = activeDeviceIndex_;
        flipX = flipX_;
        flipY = flipY_;
        d = devices_[static_cast<size_t>(deviceIndex)];
        f = desiredFormat_;

    lastPrivacyDenied_ = false;
    int reopenBudget = 3;
    CameraOpenResult openRes = openOnce();
    while (!openRes.ok && reopenBudget-- > 0 && running_ && cameraRunning_ && openRes.category == ErrorCategory::BackendFailure) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        openRes = openOnce();
    if (!openRes.ok) {
        lastPrivacyDenied_ = (openRes.category == ErrorCategory::PrivacyDenied);
        {
            std::lock_guard<std::mutex> lock(cameraSignalMu_);
            cameraSignal_ = 3;
            lastCameraOpenResult_ = openRes;
        cameraSignalCv_.notify_all();
        FrameLogEntry le;
        le.tsIso8601 = nowIso8601Local();
        {
            std::lock_guard<std::mutex> lock(mu_);
            le.cameraName = activeCameraNameUtf8_;
            le.cameraId = activeCameraIdUtf8_;
        le.errorCategory = openRes.category;
        le.errorCode = openRes.code;
        le.errorMessage = openRes.message;
        logger_.append(le);
        render_.status = "摄像头打开失败: " + openRes.code + " " + openRes.message;
        cameraRunning_ = false;
        return;

    {
        render_.status = "摄像头已打开";
    {
        std::lock_guard<std::mutex> lock(cameraSignalMu_);
        cameraSignal_ = 2;
        lastCameraOpenResult_ = openRes;
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
                le.errorCategory = rr.category;
                le.errorCode = rr.code;
                le.errorMessage = rr.message;
                logger_.append(le);
            if (consecutiveFail >= 10 && rr.category == ErrorCategory::BackendFailure) {
                camera_.close();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                const auto re = camera_.open(d.deviceId, f.width, f.height, f.fps);
                if (!re.ok) {
                    FrameLogEntry le;
                    le.tsIso8601 = nowIso8601Local();
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        le.cameraName = activeCameraNameUtf8_;
                        le.cameraId = activeCameraIdUtf8_;
                    le.errorCategory = re.category;
                    le.errorCode = re.code;
                    le.errorMessage = std::string("重连失败: ") + re.message;
                    logger_.append(le);
                    cameraRunning_ = false;
                    break;
                consecutiveFail = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        consecutiveFail = 0;
        {
            std::lock_guard<std::mutex> lock(cameraSignalMu_);
            if (cameraSignal_ != 4) cameraSignal_ = 4;
        cameraSignalCv_.notify_all();

        {
            std::lock_guard<std::mutex> lock(mu_);
            flipX = flipX_;
            flipY = flipY_;
        applyFlip(bgr, flipX, flipY);

        {
            std::lock_guard<std::mutex> lock(frameMu_);
            captureFrameCount_++;
            if (hasFrame_) overwriteDropCount_++;
            latestFrame_ = std::move(bgr);
            latestFrameTs_ = ts;
            hasFrame_ = true;
        frameCv_.notify_one();

    camera_.close();

void FramePipeline::processLoop() {
    using clock = std::chrono::steady_clock;
    auto lastFpsT = clock::now();
    std::uint64_t frames = 0;
    double fps = 0.0;
    std::uint64_t procIndex = 0;

    std::uint64_t lastCap = 0;
    std::uint64_t lastDrop = 0;
    double lastDropRate = 0.0;
    double lastInferMs = 0.0;
    std::vector<FaceMatch> lastDetections;
    // Performance optimization: Avoid redundant clone() by reusing persistent buffers.
    // Why: Reduces heap allocations and GC overhead in the hot render/inference loop.
    // Rollback: Revert to local `cv::Mat frame` and `cv::Mat draw = frame.clone()`.
    cv::Mat drawBuffer;
    cv::Mat frameBuffer;

    while (running_) {
        cv::Mat& frame = frameBuffer;
        std::uint64_t ts = 0;

        {
            std::unique_lock<std::mutex> lock(frameMu_);
            if (!running_) break;
            if (!hasFrame_) continue;
            latestFrame_.copyTo(frame);
            ts = latestFrameTs_;
            hasFrame_ = false;

        procIndex++;
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

            const bool bad = (lastInferMs > 100.0) || (lastDropRate > 0.05);
            const int prevStride = detectStride_;
            if (cfg_.dnn.enable && dnn_ && dnn_->ready()) {
                if (bad) {
                    if (detectStride_ < 8) detectStride_++;
                    if (detectStride_ > 1) detectStride_--;
            if (detectStride_ != prevStride) {
                EventLogger* ev = nullptr;
                {
                    std::lock_guard<std::mutex> lock(eventsMu_);
                    ev = events_;
                if (ev) {
                    std::ostringstream oss;
                    oss << "stride " << prevStride << "->" << detectStride_ << " infer_ms=" << lastInferMs << " drop_rate=" << lastDropRate;
                    ev->append(bad ? "stride_degrade" : "stride_recover", oss.str());

        if (clearDbRequested_.exchange(false)) {
            recognizer_.clearDb();
            recognizer_.saveDb();
            render_.status = "已清空人脸库";

        if (enrollRequested_.load()) {
            std::string pid;
            int remaining = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                pid = enrollPersonId_;
                remaining = enrollRemaining_;
            if (remaining > 0) {
                int taken = 0;
                if (recognizer_.enrollFromFrame(pid, frame, 1, taken) && taken > 0) {
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        enrollRemaining_ -= taken;
                        remaining = enrollRemaining_;
                    recognizer_.saveDb();
            if (remaining <= 0) {
                enrollRequested_ = false;
                render_.status = "注册完成: " + pid;
                render_.status = "注册中: " + pid + " 还需样本=" + std::to_string(remaining);

        std::vector<FaceMatch> matches;
        double inferMsThisFrame = 0.0;
        if (cfg_.dnn.enable && dnn_ && dnn_->ready()) {
            const bool doInfer = (detectStride_ <= 1) ? true : ((procIndex % static_cast<std::uint64_t>(detectStride_)) == 0 || lastDetections.empty());
            if (doInfer) {
                std::vector<DnnFaceDetection> dets;
                {
                    std::lock_guard<std::mutex> lock(dnnMu_);
                    dets = dnn_->detect(frame, inferMsThisFrame);
                lastInferMs = inferMsThisFrame;
                lastDetections.clear();
                lastDetections.reserve(dets.size());
                for (const auto& d : dets) {
                    FaceMatch m;
                    m.rect = d.rect;
                    m.personId = "";
                    m.distance = 0.0;
                    m.confidence = d.confidence;
                    m.accepted = false;
                    lastDetections.push_back(std::move(m));
            matches = lastDetections;
            matches = recognizer_.identify(frame);

        frame.copyTo(drawBuffer); cv::Mat& draw = drawBuffer;
        drawFacesOverlay(draw, matches);

        FrameLogEntry le;
        le.tsIso8601 = nowIso8601Local();
        {
            std::lock_guard<std::mutex> lock(mu_);
            le.cameraName = activeCameraNameUtf8_;
            le.cameraId = activeCameraIdUtf8_;
        le.frameIndex = frameIndex_++;
        le.frameWidth = frame.cols;
        le.frameHeight = frame.rows;
        le.fps = fps;
        for (const auto& m : matches) {
            FaceLogEntry fe;
            fe.x = m.rect.x;
            fe.y = m.rect.y;
            fe.w = m.rect.width;
            fe.h = m.rect.height;
            fe.personId = m.personId;
            fe.distance = m.distance;
            fe.confidence = m.confidence;
            le.faces.push_back(std::move(fe));

        logger_.append(le);

        {
            render_.bgr = std::move(draw);
            render_.faces = std::move(matches);
            render_.fps = fps;
            render_.inferMs = lastInferMs;
            render_.dropRate = lastDropRate;
            render_.stride = detectStride_;
            render_.timestamp100ns = ts;
        {
            std::lock_guard<std::mutex> lock(facesSeqMu_);
            facesSeq_++;
        facesSeqCv_.notify_all();

