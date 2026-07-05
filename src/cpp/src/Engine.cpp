/**
 * @file Engine.cpp
 * @brief Implementation of Engine class — 值流管线化编排器。
 */
#include "Engine.h"
#include "AccelerationContract.h"
#include "Config.h"
#include "Storage.h"
#include "NativeLog.h"
#include <cctype>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <algorithm>
#include <sstream>
#ifdef __linux__
#include <sys/resource.h>
#endif

#if defined(RK_HAVE_LIBYUV) && RK_HAVE_LIBYUV
#include <libyuv/convert_argb.h>
#include <libyuv/convert_from.h>
#endif

#if defined(RK_HAVE_MPP) && RK_HAVE_MPP && !defined(_WIN32)
#include <rk_mpi.h>
#include <mpp.h>
#include <mpp_err.h>
#endif

using namespace rk_core;

namespace {
static bool getEnvBool(const char* envName);
static const char* throttleModeName(InferenceThrottleMode mode);

static bool fillI420FromYuv420888(const ExternalFrame& f, cv::Mat& outYuvI420, std::string& err) {
    const int w = f.width;
    const int h = f.height;
    if (w <= 0 || h <= 0) {
        err = "非法尺寸";
        return false;
    }
    if ((w % 2) != 0 || (h % 2) != 0) {
        err = "YUV_420_888 仅支持偶数宽高";
        return false;
    }

    const int yRowStride = (f.y.rowStride > 0) ? f.y.rowStride : w;
    const int yPixelStride = (f.y.pixelStride > 0) ? f.y.pixelStride : 1;
    if (yRowStride <= 0 || yPixelStride <= 0) {
        err = "Y stride 非法";
        return false;
    }
    if (f.y.bytes.empty()) {
        err = "缺少 Y 平面";
        return false;
    }

    const int w2 = w / 2;
    const int h2 = h / 2;
    const std::size_t ySize = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    const std::size_t uvSize = static_cast<std::size_t>(w2) * static_cast<std::size_t>(h2);
    const std::size_t total = ySize + 2U * uvSize;

    outYuvI420.create(h + h2, w, CV_8UC1);
    if (!outYuvI420.data || outYuvI420.total() < total) {
        err = "I420 缓冲区分配失败";
        return false;
    }
    uint8_t* dst = outYuvI420.data;

    const std::size_t yLast =
        static_cast<std::size_t>(h - 1) * static_cast<std::size_t>(yRowStride) +
        static_cast<std::size_t>(w - 1) * static_cast<std::size_t>(yPixelStride);
    if (yRowStride < (w - 1) * yPixelStride + 1 || yLast >= f.y.bytes.size()) {
        err = "Y 平面缓冲区不足或 stride 不匹配";
        return false;
    }

    if (yPixelStride == 1) {
        if (yRowStride == w) {
            std::memcpy(dst, f.y.bytes.data(), ySize);
        } else {
            for (int row = 0; row < h; row++) {
                const std::size_t srcBase = static_cast<std::size_t>(row) * static_cast<std::size_t>(yRowStride);
                const std::size_t dstBase = static_cast<std::size_t>(row) * static_cast<std::size_t>(w);
                std::memcpy(dst + dstBase, f.y.bytes.data() + srcBase, static_cast<std::size_t>(w));
            }
        }
    } else {
        for (int row = 0; row < h; row++) {
            const uint8_t* srcPtr = f.y.bytes.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(yRowStride);
            uint8_t* dstPtr = dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(w);
            for (int col = 0; col < w; col++) {
                dstPtr[col] = *srcPtr;
                srcPtr += yPixelStride;
            }
        }
    }

    const int uRowStride = f.u.rowStride;
    const int uPixelStride = (f.u.pixelStride > 0) ? f.u.pixelStride : 1;
    const int vRowStride = f.v.rowStride;
    const int vPixelStride = (f.v.pixelStride > 0) ? f.v.pixelStride : 1;

    if (uRowStride <= 0 || vRowStride <= 0 || uPixelStride <= 0 || vPixelStride <= 0) {
        err = "U/V stride 非法";
        return false;
    }
    if (f.u.bytes.empty() || f.v.bytes.empty()) {
        err = "缺少 U/V 平面";
        return false;
    }

    const std::size_t uLast =
        static_cast<std::size_t>(h2 - 1) * static_cast<std::size_t>(uRowStride) +
        static_cast<std::size_t>(w2 - 1) * static_cast<std::size_t>(uPixelStride);
    const std::size_t vLast =
        static_cast<std::size_t>(h2 - 1) * static_cast<std::size_t>(vRowStride) +
        static_cast<std::size_t>(w2 - 1) * static_cast<std::size_t>(vPixelStride);
    if (uRowStride < (w2 - 1) * uPixelStride + 1 || uLast >= f.u.bytes.size()) {
        err = "U 平面缓冲区不足或 stride 不匹配";
        return false;
    }
    if (vRowStride < (w2 - 1) * vPixelStride + 1 || vLast >= f.v.bytes.size()) {
        err = "V 平面缓冲区不足或 stride 不匹配";
        return false;
    }

    const std::size_t uBase = ySize;
    const std::size_t vBase = ySize + uvSize;
    if (uPixelStride == 1 && vPixelStride == 1 && uRowStride == w2 && vRowStride == w2) {
        std::memcpy(dst + uBase, f.u.bytes.data(), uvSize);
        std::memcpy(dst + vBase, f.v.bytes.data(), uvSize);
    } else {
        for (int row = 0; row < h2; row++) {
            const uint8_t* uSrcPtr = f.u.bytes.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(uRowStride);
            const uint8_t* vSrcPtr = f.v.bytes.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(vRowStride);
            uint8_t* uDstPtr = dst + uBase + static_cast<std::size_t>(row) * static_cast<std::size_t>(w2);
            uint8_t* vDstPtr = dst + vBase + static_cast<std::size_t>(row) * static_cast<std::size_t>(w2);
            for (int col = 0; col < w2; col++) {
                uDstPtr[col] = *uSrcPtr;
                vDstPtr[col] = *vSrcPtr;
                uSrcPtr += uPixelStride;
                vSrcPtr += vPixelStride;
            }
        }
    }

    return true;
}

static bool toBgrFromNv21(const ExternalFrame& f, cv::Mat& outBgr, std::string& err) {
    const int w = f.width;
    const int h = f.height;
    if (w <= 0 || h <= 0) {
        err = "非法尺寸";
        return false;
    }

    const int stride = (f.nv21RowStrideY > 0) ? f.nv21RowStrideY : w;
    if (stride < w || stride <= 0) {
        err = "NV21 stride 非法";
        return false;
    }

    const int uvH = h / 2;
    const std::size_t needY = static_cast<std::size_t>(stride) * static_cast<std::size_t>(h);
    const std::size_t needUV = static_cast<std::size_t>(stride) * static_cast<std::size_t>(uvH);
    if (f.nv21.size() < needY + needUV) {
        err = "NV21 缓冲区长度不足";
        return false;
    }

    outBgr.create(h, w, CV_8UC3);

    auto fallbackOpencvNv21 = [&]() {
        if (stride == w) {
            std::vector<uint8_t> nv21Copy(f.nv21.begin(), f.nv21.end());
            cv::Mat yuv(h + uvH, w, CV_8UC1, nv21Copy.data());
            cv::cvtColor(yuv, outBgr, cv::COLOR_YUV2BGR_NV21);
            return true;
        }

        cv::Mat packedYuv;
        packedYuv.create(h + uvH, w, CV_8UC1);

        for (int row = 0; row < h; row++) {
            const std::size_t srcBase = static_cast<std::size_t>(row) * static_cast<std::size_t>(stride);
            const std::size_t dstBase = static_cast<std::size_t>(row) * static_cast<std::size_t>(w);
            std::memcpy(packedYuv.data + dstBase, f.nv21.data() + srcBase, static_cast<std::size_t>(w));
        }

        const std::size_t uvSrcStart = needY;
        const std::size_t uvDstStart = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        for (int row = 0; row < uvH; row++) {
            const std::size_t srcBase = uvSrcStart + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride);
            const std::size_t dstBase = uvDstStart + static_cast<std::size_t>(row) * static_cast<std::size_t>(w);
            std::memcpy(packedYuv.data + dstBase, f.nv21.data() + srcBase, static_cast<std::size_t>(w));
        }

        cv::cvtColor(packedYuv, outBgr, cv::COLOR_YUV2BGR_NV21);
        return true;
    };

#if defined(RK_HAVE_LIBYUV) && RK_HAVE_LIBYUV
    if (getEnvBool("RK_USE_LIBYUV")) {
    const uint8_t* srcY = f.nv21.data();
    const uint8_t* srcVu = f.nv21.data() + needY;
    const int r = libyuv::NV21ToRGB24(
        srcY,
        stride,
        srcVu,
        stride,
        outBgr.data,
        static_cast<int>(outBgr.step),
        w,
        h
    );
    if (r != 0) {
        return fallbackOpencvNv21();
    }
    return true;
    }
    return fallbackOpencvNv21();
#else
    return fallbackOpencvNv21();
#endif
}

static bool toBgrFromExternalFrame(const ExternalFrame& f, cv::Mat& outBgr, std::string& err) {
    if (f.width <= 0 || f.height <= 0) {
        err = "非法尺寸";
        return false;
    }

    const int rot = ((f.meta.rotationDegrees % 360) + 360) % 360;
    if (f.width > 1920 || f.height > 1920) {
        err = "超规格输入: " + std::to_string(f.width) + "x" + std::to_string(f.height);
        return false;
    }

    if (f.format == ExternalFrameFormat::NV21) {
        if (!toBgrFromNv21(f, outBgr, err)) {
            return false;
        }
    } else {
        cv::Mat yuvI420;
        if (!fillI420FromYuv420888(f, yuvI420, err)) {
            return false;
        }
        outBgr.create(f.height, f.width, CV_8UC3);
        bool converted = false;
#if defined(RK_HAVE_LIBYUV) && RK_HAVE_LIBYUV
        if (getEnvBool("RK_USE_LIBYUV")) {
            const int w = f.width;
            const int h = f.height;
            const int w2 = w / 2;
            const int h2 = h / 2;
            const std::size_t ySize = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
            const std::size_t uvSize = static_cast<std::size_t>(w2) * static_cast<std::size_t>(h2);
            const uint8_t* y = yuvI420.data;
            const uint8_t* u = yuvI420.data + ySize;
            const uint8_t* v = yuvI420.data + ySize + uvSize;
            const int r = libyuv::I420ToRGB24(
                y,
                w,
                u,
                w2,
                v,
                w2,
                outBgr.data,
                static_cast<int>(outBgr.step),
                w,
                h
            );
            converted = (r == 0);
        }
#endif
        if (!converted) {
            cv::cvtColor(yuvI420, outBgr, cv::COLOR_YUV2BGR_I420);
        }
    }

    if (rot == 90) {
        cv::rotate(outBgr, outBgr, cv::ROTATE_90_CLOCKWISE);
    } else if (rot == 180) {
        cv::rotate(outBgr, outBgr, cv::ROTATE_180);
    } else if (rot == 270) {
        cv::rotate(outBgr, outBgr, cv::ROTATE_90_COUNTERCLOCKWISE);
    }
    return true;
}

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// [Qualcomm/MPP Backend Feature Placeholder]
// 待补充代码接入: Qualcomm SDK inference delegate initialization
static bool initQualcommDelegate(bool requested) {
#if defined(RK_HAVE_QUALCOMM) && RK_HAVE_QUALCOMM
    if (!requested) {
        rklog::logInfo("Engine", "initQualcommDelegate", "Qualcomm SDK disabled by user config.");
        return false;
    }
    rklog::logInfo("Engine", "initQualcommDelegate", "Qualcomm SDK detected, preparing to load backend...");
    return true;
#else
    rklog::logInfo("Engine", "initQualcommDelegate", "Qualcomm SDK fallback to CPU... (RK_HAVE_QUALCOMM not defined or 0)");
    return false;
#endif
}

static bool initMppDecoder(bool requested) {
#if defined(RK_HAVE_MPP) && RK_HAVE_MPP && !defined(_WIN32)
    if (!requested) {
        rklog::logInfo("Engine", "initMppDecoder", "RK MPP disabled by user config.");
        return false;
    }
    rklog::logInfo("Engine", "initMppDecoder", "RK MPP detected, probing decoder...");
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MPP_RET ret = mpp_create(&ctx, &mpi);
    if (ret == MPP_OK && ctx) {
        mpp_destroy(ctx);
        rklog::logInfo("Engine", "initMppDecoder", "MPP hardware decoder probe successful");
        return true;
    }
    rklog::logWarn("Engine", "initMppDecoder", "MPP probe failed: ret=" + std::to_string(ret) + ", fallback to CPU");
    return false;
#else
    (void)requested;
    rklog::logInfo("Engine", "initMppDecoder", "MPP hardware decoding fallback to CPU... (unsupported platform or RK_HAVE_MPP not defined or 0)");
    return false;
#endif
}

static bool getEnvBool(const char* envName) {
    if (const char* envVal = std::getenv(envName)) {
        std::string s;
        for (char c : std::string(envVal)) {
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return (s == "1" || s == "true" || s == "yes" || s == "on");
    }
    return false;
}

static const char* throttleModeName(InferenceThrottleMode mode) {
    switch (mode) {
        case InferenceThrottleMode::Off:
            return "off";
        case InferenceThrottleMode::Manual:
            return "manual";
        case InferenceThrottleMode::Auto:
            return "auto";
    }
    return "off";
}

static void logAccelSelfCheckStatus(const rk_accel::AccelContractStatus& status) {
    rklog::logInfo("Engine", "performAccelSelfCheck", rk_accel::formatSelfCheckLine(status));
}
}  // namespace

Engine::Engine()
    : Engine(std::make_unique<VideoManager>(),
             std::make_unique<BioAuth>(),
             std::make_unique<EventManager>()) {
}

Engine::Engine(std::unique_ptr<VideoManager> vm,
               std::unique_ptr<BioAuth> ba,
               std::unique_ptr<EventManager> em)
    : isRunning_(false),
      currentMode_(MonitoringMode::CONTINUOUS),
      externalInputEnabled_(false) {
    videoManager_ = std::move(vm);
    bioAuth_ = std::move(ba);
    eventManager_ = std::move(em);

    bool useQualcomm = getEnvBool("RK_USE_QUALCOMM");
    bool useMpp = getEnvBool("RK_USE_MPP");

    initQualcommDelegate(useQualcomm);
    initMppDecoder(useMpp);

    bool useOpencl = false;
    if (const char* envOcl = std::getenv("RK_USE_OPENCL")) {
        std::string s;
        for (char c : std::string(envOcl)) {
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        useOpencl = (s == "1" || s == "true" || s == "yes" || s == "on");
    } else {
        rklog::logInfo("Engine", "initOpenCL", "RK_USE_OPENCL not set. Defaulting to conservative switch (false).");
    }
    videoManager_->setUseOpenCL(useOpencl);

    motionDetector_ = std::make_unique<MotionDetector>();
    externalInput_ = std::make_unique<FrameInputChannel>();
    externalInput_->configure(FrameBackpressureMode::LatestOnly, 1);

    // Pipeline 组件
    trackCoordinator_ = std::make_unique<pipeline::TrackCoordinator>();
    publisher_ = std::make_unique<pipeline::ResultPublisher>();
    perfReporter_ = std::make_unique<pipeline::PerfReporter>();
}

Engine::~Engine() {
    stop();
}

void Engine::updateInferenceThrottle(const std::string& mode, int intervalMs) {
    const auto m = parseInferenceThrottleMode(mode);
    detThrottleMode_.store(m);
    detIntervalMs_.store(clampDetectionIntervalMs(intervalMs));
    recThrottleMode_.store(m);
    recIntervalMs_.store(clampRecognitionIntervalMs(intervalMs));
    rklog::logInfo(
        "Engine",
        "updateInferenceThrottle",
        rk_accel::formatSelfCheckLine({
            "detection_throttle",
            m != InferenceThrottleMode::Off,
            m != InferenceThrottleMode::Off,
            "ok",
            "mode=" + std::string(throttleModeName(m)) + " interval_ms=" + std::to_string(detIntervalMs_.load())
        }));
    rklog::logInfo(
        "Engine",
        "updateInferenceThrottle",
        rk_accel::formatSelfCheckLine({
            "recognition_throttle",
            m != InferenceThrottleMode::Off,
            m != InferenceThrottleMode::Off,
            "ok",
            "mode=" + std::string(throttleModeName(m)) + " interval_ms=" + std::to_string(recIntervalMs_.load())
        }));
}

InferenceThrottleMode Engine::getInferenceThrottleMode() const {
    return recThrottleMode_.load();
}

int Engine::getInferenceIntervalMs() const {
    return recIntervalMs_.load();
}

void Engine::updateDetectionThrottle(const std::string& mode, int intervalMs) {
    detThrottleMode_.store(parseInferenceThrottleMode(mode));
    detIntervalMs_.store(clampDetectionIntervalMs(intervalMs));
    const auto current = detThrottleMode_.load();
    rklog::logInfo(
        "Engine",
        "updateDetectionThrottle",
        rk_accel::formatSelfCheckLine({
            "detection_throttle",
            current != InferenceThrottleMode::Off,
            current != InferenceThrottleMode::Off,
            "ok",
            "mode=" + std::string(throttleModeName(current)) + " interval_ms=" + std::to_string(detIntervalMs_.load())
        }));
}

InferenceThrottleMode Engine::getDetectionThrottleMode() const {
    return detThrottleMode_.load();
}

int Engine::getDetectionIntervalMs() const {
    return detIntervalMs_.load();
}

void Engine::updateRecognitionThrottle(const std::string& mode, int intervalMs) {
    recThrottleMode_.store(parseInferenceThrottleMode(mode));
    recIntervalMs_.store(clampRecognitionIntervalMs(intervalMs));
    const auto current = recThrottleMode_.load();
    rklog::logInfo(
        "Engine",
        "updateRecognitionThrottle",
        rk_accel::formatSelfCheckLine({
            "recognition_throttle",
            current != InferenceThrottleMode::Off,
            current != InferenceThrottleMode::Off,
            "ok",
            "mode=" + std::string(throttleModeName(current)) + " interval_ms=" + std::to_string(recIntervalMs_.load())
        }));
}

InferenceThrottleMode Engine::getRecognitionThrottleMode() const {
    return recThrottleMode_.load();
}

int Engine::getRecognitionIntervalMs() const {
    return recIntervalMs_.load();
}

bool Engine::initCommon(const std::string& cascadePath, const std::string& storagePath) {
    if (initialized_.exchange(true)) {
        rklog::logWarn("Engine", __func__, "Engine 已经初始化，跳过重复调用");
        return true;
    }
    RKLOG_ENTER("Engine");
    initCancelRequested_.store(false);
    videoManager_->setCancelToken(&initCancelRequested_);
    std::string base = storagePath;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') {
        base.append("/");
    }
    this->storagePath_ = base + "cache/";

    if (!Storage::ensureDirectory(this->storagePath_)) {
        std::cerr << "Failed to init storage: " << this->storagePath_ << std::endl;
        rklog::logError("Engine", __func__, "Failed to init storage");
        return false;
    }
    Storage::cleanupOldData(this->storagePath_, rk_core::config::OFFLINE_CACHE_DAYS);
    if (!bioAuth_->initialize(cascadePath)) {
        rklog::logError("Engine", __func__, "Failed to init BioAuth with cascade: " + cascadePath);
        return false;
    }
    return true;
}

bool Engine::initialize(int cameraId, const std::string& cascadePath, const std::string& storagePath) {
    if (!initCommon(cascadePath, storagePath)) return false;
    if (cameraId >= 0) {
        if (initCancelRequested_.load()) return false;
        if (!videoManager_->open(cameraId)) {
            std::cerr << "Failed to open camera " << cameraId << "." << std::endl;
            rklog::logError("Engine", __func__, "Failed to open camera");
            return false;
        }
    } else {
        rklog::logInfo("Engine", __func__, "cameraId<0：跳过 VideoManager 相机打开（预期用于外部帧输入）");
    }
    performAccelSelfCheck();
    return true;
}

bool Engine::initialize(const std::string& filePath, const std::string& cascadePath, const std::string& storagePath) {
    if (!initCommon(cascadePath, storagePath)) return false;
    if (initCancelRequested_.load()) return false;
    if (!videoManager_->open(filePath)) {
        std::cerr << "Failed to open mock file: " << filePath << std::endl;
        rklog::logError("Engine", __func__, "Failed to open mock file reason=" + videoManager_->getLastMockRejectReason());
        return false;
    }
    if (initCancelRequested_.load()) {
        videoManager_->close();
        return false;
    }
    performAccelSelfCheck();
    return true;
}

void Engine::requestCancelInit() {
    initCancelRequested_.store(true);
}

void Engine::clearCancelInit() {
    initCancelRequested_.store(false);
}

void Engine::run() {
    RKLOG_ENTER("Engine");
    isRunning_ = true;
    cv::Mat frame;

    std::cout << "Engine started." << std::endl;

    while (isRunning_) {
        // 1. 取帧：优先消费外部帧通道，其次回退到 VideoManager
        double decodeMs = 0.0;
        bool processed = false;

        if (externalInputEnabled_.load() && externalInput_) {
            ExternalFrame ef;
            if (externalInput_->waitPop(ef, 30)) {
                std::string err;
                if (toBgrFromExternalFrame(ef, frame, err)) {
                    decodeMs = 0.0;  // 外部帧不计解码耗时
                    processed = true;
                } else {
                    rklog::logWarn("Engine", __func__, "外部帧转换失败: " + err + " / " + ef.brief());
                }
            }
        }
        if (!processed && videoManager_->getLatestFrame(frame)) {
            processed = true;
        }

        if (!processed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // 2. Motion gate：MOTION_TRIGGERED 模式下仅在有运动时执行管线
        if (currentMode_ == MonitoringMode::MOTION_TRIGGERED &&
            !motionDetector_->detect(frame)) {
            pipeline::FrameOutcome outcome;
            outcome.renderFrame = frame.clone();
            cv::putText(outcome.renderFrame, "WAITING", cv::Point(20, 40),
                cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);
            publisher_->publish(outcome);
            continue;
        }

        // 3. 预处理：resize + flip（内联，避免额外函数调用开销）
        cv::Mat processed2;
        if (frame.cols != rk_core::config::FRAME_WIDTH || frame.rows != rk_core::config::FRAME_HEIGHT) {
            cv::resize(frame, processed2, cv::Size(rk_core::config::FRAME_WIDTH, rk_core::config::FRAME_HEIGHT),
                       0.0, 0.0, cv::INTER_AREA);
        } else {
            processed2 = frame;
        }
        const bool fx = flipXEnabled_.load();
        const bool fy = flipYEnabled_.load();
        if (fx || fy) {
            int code = (fx && fy) ? -1 : (fy ? 0 : 1);
            cv::flip(processed2, processed2, code);
        }

        // 4. 检测 + 识别（委托 BioAuth）
        std::vector<BioAuth::FaceAuthResult> results;
        bioAuth_->verifyMulti(processed2, results, 4, true);

        // 5. 转化为 DetectedFace + 跟踪（委托 TrackCoordinator）
        std::vector<pipeline::DetectedFace> faces;
        faces.reserve(results.size());
        for (auto& r : results) {
            pipeline::DetectedFace df;
            df.bbox = r.face;
            df.identityId = r.identity.id;
            df.confidence = r.identity.confidence;
            df.isAuthenticated = r.identity.isAuthenticated;
            faces.push_back(df);
        }
        auto tracks = trackCoordinator_->update(faces, nowMs());

        // 6. 组装 FrameOutcome
        pipeline::FrameOutcome outcome;
        outcome.tracks = tracks;
        outcome.renderFrame = processed2.clone();
        outcome.stats.decodeMs = decodeMs;

        // 7. 副作用：回调 + 渲染帧持久化 + 性能统计
        publisher_->publish(outcome);
        perfReporter_->submit(outcome.stats);
    }
}

void Engine::stop() {
    initialized_.store(false);
    RKLOG_ENTER("Engine");
    isRunning_ = false;
    if (videoManager_) videoManager_->close();
    if (externalInput_) {
        externalInput_->clear();
    }
}

void Engine::setMode(MonitoringMode mode) {
    RKLOG_ENTER("Engine");
    currentMode_ = mode;
    std::cout << "Switched to mode: " << (mode == MonitoringMode::CONTINUOUS ? "Continuous" : "Motion") << std::endl;
}

void Engine::setExternalInputEnabled(bool enabled) {
    RKLOG_ENTER("Engine");
    externalInputEnabled_.store(enabled);
    if (externalInput_ && !enabled) {
        externalInput_->clear();
    }
    rklog::logInfo("Engine", __func__, std::string("外部帧输入通道 ") + (enabled ? "已启用" : "已禁用"));
}

void Engine::configureExternalInput(FrameBackpressureMode mode, std::size_t capacity) {
    RKLOG_ENTER("Engine");
    if (!externalInput_) {
        externalInput_ = std::make_unique<FrameInputChannel>();
    }
    externalInput_->configure(mode, capacity);
    rklog::logInfo("Engine", __func__, "外部帧背压配置: mode=" + std::to_string(static_cast<int>(mode)) +
        " capacity=" + std::to_string(externalInput_->capacity()));
}

bool Engine::pushExternalFrame(ExternalFrame frame) {
    if (!externalInputEnabled_.load()) {
        return false;
    }
    if (!externalInput_) {
        externalInput_ = std::make_unique<FrameInputChannel>();
    }
    externalInput_->push(std::move(frame));
    return true;
}

bool Engine::getRenderFrame(cv::Mat& outFrame) {
    uint64_t seq = 0;
    return getRenderFrame(outFrame, seq);
}

bool Engine::getRenderFrame(cv::Mat& outFrame, uint64_t& inOutSeq) {
    if (!publisher_) return false;
    return publisher_->getRenderFrame(outFrame, inOutSeq);
}

void Engine::setFlip(bool flipX, bool flipY) {
    flipXEnabled_.store(flipX);
    flipYEnabled_.store(flipY);
}

void Engine::setOnResultCallback(std::function<void(std::string)> callback) {
    if (publisher_) {
        publisher_->setCallback(std::move(callback));
    }
}

void Engine::performAccelSelfCheck() {
    RKLOG_ENTER("Engine");

    // ncnn
    logAccelSelfCheckStatus({
        "ncnn",
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
        true,
        true,
        "ok",
        "RK_HAVE_NCNN=1"
#else
        false,
        false,
        "build_disabled",
        "RK_HAVE_NCNN=0"
#endif
    });

    // MPP
    {
        const bool requested = getEnvBool("RK_USE_MPP");
        const bool effective =
#if defined(RK_HAVE_MPP) && RK_HAVE_MPP && !defined(_WIN32)
            requested;
#else
            false;
#endif
        logAccelSelfCheckStatus({
            "mpp",
            requested,
            effective,
            effective ? "ok" :
#if defined(_WIN32)
                "unsupported_platform",
#elif defined(RK_HAVE_MPP) && RK_HAVE_MPP
                (requested ? "runtime_init_failed" : "build_disabled"),
#else
                (requested ? "missing_dependency" : "build_disabled"),
#endif
#if defined(RK_HAVE_MPP) && RK_HAVE_MPP && !defined(_WIN32)
            "RK_HAVE_MPP=1"
#elif defined(_WIN32)
            "platform=windows"
#else
            "RK_HAVE_MPP=0"
#endif
        });
    }

    // Qualcomm
    {
        const bool requested = getEnvBool("RK_USE_QUALCOMM");
        const bool effective =
#if defined(RK_HAVE_QUALCOMM) && RK_HAVE_QUALCOMM
            requested;
#else
            false;
#endif
        logAccelSelfCheckStatus({
            "qualcomm",
            requested,
            effective,
            effective ? "ok" : (requested ? "missing_dependency" : "build_disabled"),
#if defined(RK_HAVE_QUALCOMM) && RK_HAVE_QUALCOMM
            "RK_HAVE_QUALCOMM=1"
#else
            "RK_HAVE_QUALCOMM=0"
#endif
        });
    }

    // OpenCL
    bool oclRequested = false;
    if (const char* envOcl = std::getenv("RK_USE_OPENCL")) {
        std::string s;
        for (char c : std::string(envOcl)) {
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        oclRequested = (s == "1" || s == "true" || s == "yes" || s == "on");
    }
    bool oclEffective = cv::ocl::useOpenCL();
    bool oclHave = cv::ocl::haveOpenCL();
    std::string oclEvidence = "useOpenCL=" + std::to_string(oclEffective) +
        " haveOpenCL=" + std::to_string(oclHave);
    if (oclHave) {
        try {
            cv::ocl::Device dev = cv::ocl::Device::getDefault();
            oclEvidence += " device=" + dev.name() + " vendor=" + dev.vendorName();
        } catch (const std::exception& e) {
            rklog::logWarn("Engine", "performAccelSelfCheck", "OpenCL device info 获取异常: " + std::string(e.what()));
        } catch (...) {
            rklog::logWarn("Engine", "performAccelSelfCheck", "OpenCL device info 获取异常 (unknown)");
        }
    }
    logAccelSelfCheckStatus({
        "opencl",
        oclRequested,
        oclEffective,
        oclEffective ? "ok" : (oclRequested ? "missing_dependency" : "build_disabled"),
        oclEvidence
    });

    // libyuv
    {
        const bool requested = getEnvBool("RK_USE_LIBYUV");
        const bool effective =
#if defined(RK_HAVE_LIBYUV) && RK_HAVE_LIBYUV
            requested;
#else
            false;
#endif
        logAccelSelfCheckStatus({
            "libyuv",
            requested,
            effective,
            effective ? "ok" : (requested ? "missing_dependency" : "build_disabled"),
#if defined(RK_HAVE_LIBYUV) && RK_HAVE_LIBYUV
            "RK_HAVE_LIBYUV=1"
#else
            "RK_HAVE_LIBYUV=0"
#endif
        });
    }
}
