/**
 * @file Engine.cpp
 * @brief Implementation of Engine class.
 */
#include "Engine.h"
#include "Config.h"
#include "Storage.h"
#include "NativeLog.h"
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {
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
            const std::size_t srcBase = static_cast<std::size_t>(row) * static_cast<std::size_t>(yRowStride);
            const std::size_t dstBase = static_cast<std::size_t>(row) * static_cast<std::size_t>(w);
            for (int col = 0; col < w; col++) {
                dst[dstBase + static_cast<std::size_t>(col)] =
                    f.y.bytes[srcBase + static_cast<std::size_t>(col) * static_cast<std::size_t>(yPixelStride)];
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
            const std::size_t uSrcBase = static_cast<std::size_t>(row) * static_cast<std::size_t>(uRowStride);
            const std::size_t vSrcBase = static_cast<std::size_t>(row) * static_cast<std::size_t>(vRowStride);
            const std::size_t uvDstBase = static_cast<std::size_t>(row) * static_cast<std::size_t>(w2);
            for (int col = 0; col < w2; col++) {
                dst[uBase + uvDstBase + static_cast<std::size_t>(col)] =
                    f.u.bytes[uSrcBase + static_cast<std::size_t>(col) * static_cast<std::size_t>(uPixelStride)];
                dst[vBase + uvDstBase + static_cast<std::size_t>(col)] =
                    f.v.bytes[vSrcBase + static_cast<std::size_t>(col) * static_cast<std::size_t>(vPixelStride)];
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

    if (stride == w) {
        cv::Mat yuv(h + uvH, w, CV_8UC1, const_cast<uint8_t*>(f.nv21.data()));
        cv::cvtColor(yuv, outBgr, cv::COLOR_YUV2BGR_NV21);
        return true;
    }

    thread_local cv::Mat packedYuv;
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
}

static bool toBgrFromExternalFrame(const ExternalFrame& f, cv::Mat& outBgr, std::string& err) {
    if (f.width <= 0 || f.height <= 0) {
        err = "非法尺寸";
        return false;
    }

    const int rot = ((f.meta.rotationDegrees % 360) + 360) % 360;
    int effectiveW = f.width;
    int effectiveH = f.height;
    if (rot == 90 || rot == 270) {
        std::swap(effectiveW, effectiveH);
    }
    if (effectiveW > 1920 || effectiveH > 1080) {
        err = "超规格输入: " + std::to_string(effectiveW) + "x" + std::to_string(effectiveH);
        return false;
    }

    if (f.format == ExternalFrameFormat::NV21) {
        if (!toBgrFromNv21(f, outBgr, err)) {
            return false;
        }
    } else {
        thread_local cv::Mat yuvI420;
        if (!fillI420FromYuv420888(f, yuvI420, err)) {
            return false;
        }
        outBgr.create(f.height, f.width, CV_8UC3);
        cv::cvtColor(yuvI420, outBgr, cv::COLOR_YUV2BGR_I420);
    }

    if (f.meta.mirrored) {
        cv::flip(outBgr, outBgr, 1);
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
}  // namespace

Engine::Engine() 
    : isRunning(false),
      currentMode(MonitoringMode::CONTINUOUS),
      externalInputEnabled(false),
      frameCount(0),
      lastStatTime(0) {
    videoManager = std::make_unique<VideoManager>();
    motionDetector = std::make_unique<MotionDetector>();
    bioAuth = std::make_unique<BioAuth>();
    eventManager = std::make_unique<EventManager>();
    externalInput = std::make_unique<FrameInputChannel>();
    externalInput->configure(FrameBackpressureMode::LatestOnly, 1);
}

Engine::~Engine() {
    stop();
}

bool Engine::initialize(int cameraId, const std::string& cascadePath, const std::string& storagePath) {
    RKLOG_ENTER("Engine");
    std::string base = storagePath;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') {
        base.append("/");
    }
    this->storagePath = base + "cache/";

    // 1. Ensure storage
    if (!Storage::ensureDirectory(this->storagePath)) {
        std::cerr << "Failed to init storage: " << this->storagePath << std::endl;
        rklog::logError("Engine", __func__, "Failed to init storage");
        return false;
    }
    
    // 2. Cleanup old data
    Storage::cleanupOldData(this->storagePath, Config::OFFLINE_CACHE_DAYS);

    // 3. Init BioAuth
    if (!bioAuth->initialize(cascadePath)) {
        rklog::logError("Engine", __func__, "Failed to init BioAuth with cascade: " + cascadePath);
    }

    if (cameraId >= 0) {
        if (!videoManager->open(cameraId)) {
            std::cerr << "Failed to open camera " << cameraId << "." << std::endl;
            rklog::logError("Engine", __func__, "Failed to open camera");
            return false;
        }
    } else {
        rklog::logInfo("Engine", __func__, "cameraId<0：跳过 VideoManager 相机打开（预期用于外部帧输入）");
    }

    return true;
}

bool Engine::initialize(const std::string& filePath, const std::string& cascadePath, const std::string& storagePath) {
    RKLOG_ENTER("Engine");
    std::string base = storagePath;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') {
        base.append("/");
    }
    this->storagePath = base + "cache/";

    // 1. Ensure storage
    if (!Storage::ensureDirectory(this->storagePath)) {
        std::cerr << "Failed to init storage: " << this->storagePath << std::endl;
        rklog::logError("Engine", __func__, "Failed to init storage");
        return false;
    }
    
    // 2. Cleanup old data
    Storage::cleanupOldData(this->storagePath, Config::OFFLINE_CACHE_DAYS);

    // 3. Init BioAuth
    if (!bioAuth->initialize(cascadePath)) {
        rklog::logError("Engine", __func__, "Failed to init BioAuth with cascade: " + cascadePath);
    }

    // 4. Init Mock Source
    if (!videoManager->open(filePath)) {
        std::cerr << "Failed to open mock file: " << filePath << std::endl;
        rklog::logError("Engine", __func__, "Failed to open mock file");
        return false;
    }

    return true;
}

void Engine::run() {
    RKLOG_ENTER("Engine");
    isRunning = true;
    cv::Mat frame;

    std::cout << "Engine started." << std::endl;

    while (isRunning) {
        // 外部帧输入：优先消费外部通道，必要时再回退到 VideoManager（便于在采集失败时仍能跑通 UI/推理链路）。
        bool processed = false;
        if (externalInputEnabled.load()) {
            ExternalFrame ef;
            if (externalInput && externalInput->waitPop(ef, 30)) {
                cv::Mat bgr;
                std::string err;
                if (toBgrFromExternalFrame(ef, bgr, err)) {
                    processFrame(bgr);
                    processed = true;
                } else {
                    rklog::logWarn("Engine", __func__, "外部帧转换失败: " + err + " / " + ef.brief());
                }
            }
        }

        if (!processed && videoManager->getLatestFrame(frame)) {
            processFrame(frame);
            processed = true;
        }

        if (processed) {
            frameCount++;
            totalFrames++;
            long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now - lastStatTime > 1000) {
                frameCount = 0;
                lastStatTime = now;
            }
            if (maxFrames > 0 && totalFrames >= maxFrames) {
                std::cout << "Reached max frames (" << maxFrames << "). Stopping." << std::endl;
                stop();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void Engine::stop() {
    RKLOG_ENTER("Engine");
    isRunning = false;
    videoManager->close();
    if (externalInput) {
        externalInput->clear();
    }
}

void Engine::setMode(MonitoringMode mode) {
    RKLOG_ENTER("Engine");
    currentMode = mode;
    std::cout << "Switched to mode: " << (mode == MonitoringMode::CONTINUOUS ? "Continuous" : "Motion") << std::endl;
}

void Engine::setExternalInputEnabled(bool enabled) {
    RKLOG_ENTER("Engine");
    externalInputEnabled.store(enabled);
    if (externalInput && !enabled) {
        externalInput->clear();
    }
    rklog::logInfo("Engine", __func__, std::string("外部帧输入通道 ") + (enabled ? "已启用" : "已禁用"));
}

void Engine::configureExternalInput(FrameBackpressureMode mode, std::size_t capacity) {
    RKLOG_ENTER("Engine");
    if (!externalInput) {
        externalInput = std::make_unique<FrameInputChannel>();
    }
    externalInput->configure(mode, capacity);
    rklog::logInfo("Engine", __func__, "外部帧背压配置: mode=" + std::to_string(static_cast<int>(mode)) +
        " capacity=" + std::to_string(externalInput->capacity()));
}

bool Engine::pushExternalFrame(ExternalFrame frame) {
    if (!externalInputEnabled.load()) {
        return false;
    }
    if (!externalInput) {
        externalInput = std::make_unique<FrameInputChannel>();
    }
    externalInput->push(std::move(frame));
    return true;
}

void Engine::processFrame(const cv::Mat& inputFrame) {
    cv::Mat frame;
    if (inputFrame.cols != Config::FRAME_WIDTH || inputFrame.rows != Config::FRAME_HEIGHT) {
        cv::resize(inputFrame, frame, cv::Size(Config::FRAME_WIDTH, Config::FRAME_HEIGHT), 0.0, 0.0, cv::INTER_AREA);
    } else {
        frame = inputFrame;
    }

    cv::Mat debugFrame = frame.clone();

    // 1. Motion Detection check for Non-Continuous mode
    if (currentMode == MonitoringMode::MOTION_TRIGGERED) {
        if (!motionDetector->detect(frame)) {
            // Even if no motion, we update render frame
            std::lock_guard<std::mutex> lock(renderMutex);
            renderFrame = debugFrame;
            return; 
        }
        // Visualize motion (optional: draw contours)
        cv::putText(debugFrame, "MOTION DETECTED", cv::Point(20, 40), 
            cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);
    }

    // 2. Biometric Authentication
    PersonIdentity identity;
    std::vector<cv::Rect> faces;
    cv::Rect mainFace;
    bool faceDetected = bioAuth->verify(frame, identity, faces, mainFace);
    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    if (faceDetected) {
        cv::Scalar color = identity.isAuthenticated ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
        for (const auto& r : faces) {
            const bool isMain = (r.x == mainFace.x && r.y == mainFace.y && r.width == mainFace.width && r.height == mainFace.height);
            cv::Scalar c = isMain ? color : cv::Scalar(0, 255, 255);
            int thickness = isMain ? 3 : 2;
            cv::rectangle(debugFrame, r, c, thickness);
        }
        
        std::string statusText = identity.isAuthenticated ? 
            "Verified: " + identity.id : "Unknown";
        
        cv::putText(debugFrame, statusText, cv::Point(20, 80), 
            cv::FONT_HERSHEY_SIMPLEX, 0.8, color, 2);

        if (identity.isAuthenticated) {
            std::cout << "User Verified: " << identity.id << " (" << identity.confidence << ")" << std::endl;
            if (onResultCallback && (now - lastVerifiedMs > 800)) {
                lastVerifiedMs = now;
                std::string msg = "VERIFIED " + identity.id + " " + std::to_string((int)(identity.confidence * 100)) + "%";
                onResultCallback(msg);
            }
        } else {
            // 3. Abnormal Event: Unknown person
            handleAbnormalEvent("AUTH_FAIL", "Unknown person detected", frame);
            if (onResultCallback && (now - lastUnknownMs > 1200)) {
                lastUnknownMs = now;
                onResultCallback("AUTH_FAIL Unknown");
            }
        }
    } else {
        if (onResultCallback && (now - lastNoFaceMs > 2000)) {
            lastNoFaceMs = now;
            onResultCallback("NO_FACE");
        }
    }

    // Update the render frame safely
    {
        std::lock_guard<std::mutex> lock(renderMutex);
        renderFrame = debugFrame;
    }
}

bool Engine::getRenderFrame(cv::Mat& outFrame) {
    std::lock_guard<std::mutex> lock(renderMutex);
    if (renderFrame.empty()) return false;
    renderFrame.copyTo(outFrame);
    return true;
}

void Engine::handleAbnormalEvent(const std::string& type, const std::string& desc, const cv::Mat& evidence) {
    RKLOG_ENTER("Engine");
    std::string timestamp = std::to_string(std::time(nullptr));
    std::string imgPath = this->storagePath + type + "_" + timestamp + ".jpg";
    
    // Save evidence asynchronously or quickly
    if (Storage::saveImage(imgPath, evidence)) {
        eventManager->logEvent(type, desc, imgPath);
        std::cout << "Event Logged: " << desc << std::endl;
    }
}
