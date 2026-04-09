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
#include <algorithm>
#include <sstream>

#if defined(RK_HAVE_LIBYUV) && RK_HAVE_LIBYUV
#include <libyuv/convert_argb.h>
#include <libyuv/convert_from.h>
#endif

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

#if defined(RK_HAVE_LIBYUV) && RK_HAVE_LIBYUV
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
        err = "libyuv NV21ToRGB24 失败";
        return false;
    }
    return true;
#else
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
#endif
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
#if defined(RK_HAVE_LIBYUV) && RK_HAVE_LIBYUV
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
        if (r != 0) {
            err = "libyuv I420ToRGB24 失败";
            return false;
        }
#else
        cv::cvtColor(yuvI420, outBgr, cv::COLOR_YUV2BGR_I420);
#endif
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

static float iou(const cv::Rect& a, const cv::Rect& b) {
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);
    const int w = std::max(0, x2 - x1);
    const int h = std::max(0, y2 - y1);
    const float inter = static_cast<float>(w * h);
    const float uni = static_cast<float>(a.area() + b.area()) - inter;
    return (uni <= 0.0f) ? 0.0f : (inter / uni);
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
    initCancelRequested.store(false);
    videoManager->setCancelToken(&initCancelRequested);
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
        if (initCancelRequested.load()) {
            return false;
        }
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
    initCancelRequested.store(false);
    videoManager->setCancelToken(&initCancelRequested);
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
    if (initCancelRequested.load()) {
        return false;
    }
    if (!videoManager->open(filePath)) {
        std::cerr << "Failed to open mock file: " << filePath << std::endl;
        rklog::logError("Engine", __func__, "Failed to open mock file");
        return false;
    }
    if (initCancelRequested.load()) {
        videoManager->close();
        return false;
    }

    return true;
}

void Engine::requestCancelInit() {
    initCancelRequested.store(true);
}

void Engine::clearCancelInit() {
    initCancelRequested.store(false);
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
    const bool fx = flipXEnabled.load();
    const bool fy = flipYEnabled.load();
    if (fx || fy) {
        int code = 1;
        if (fx && fy) code = -1;
        else if (fy) code = 0;
        else code = 1;
        cv::flip(debugFrame, debugFrame, code);
    }
    frame = debugFrame;

    // 1. Motion Detection check for Non-Continuous mode
    if (currentMode == MonitoringMode::MOTION_TRIGGERED) {
        if (!motionDetector->detect(frame)) {
            // Even if no motion, we update render frame
            std::lock_guard<std::mutex> lock(renderMutex);
            renderFrame = debugFrame;
            renderFrameSeq++;
            return; 
        }
        // Visualize motion (optional: draw contours)
        cv::putText(debugFrame, "MOTION DETECTED", cv::Point(20, 40), 
            cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);
    }

    // 2. Multi-face authentication + tracking
    std::vector<BioAuth::FaceAuthResult> results;
    bool faceDetected = bioAuth->verifyMulti(frame, results, 4);
    long long now = nowMs();

    const long long trackTtlMs = 1200;
    const float matchIouThreshold = 0.3f;
    const int stableFrames = 3;

    for (auto& t : faceTracks) {
        if (now - t.lastSeenMs > trackTtlMs) {
            t.lastSeenMs = 0;
        }
    }
    faceTracks.erase(
        std::remove_if(faceTracks.begin(), faceTracks.end(), [](const FaceTrack& t) { return t.lastSeenMs == 0; }),
        faceTracks.end()
    );

    std::vector<int> detOrder(results.size());
    for (int i = 0; i < static_cast<int>(results.size()); i++) detOrder[i] = i;
    std::sort(detOrder.begin(), detOrder.end(), [&](int a, int b) {
        return results[a].face.area() > results[b].face.area();
    });

    std::vector<bool> trackUsed(faceTracks.size(), false);
    std::vector<int> detToTrack(results.size(), -1);

    for (int idx : detOrder) {
        float best = 0.0f;
        int bestTrack = -1;
        for (int ti = 0; ti < static_cast<int>(faceTracks.size()); ti++) {
            if (trackUsed[ti]) continue;
            float s = iou(faceTracks[ti].bbox, results[idx].face);
            if (s > best) {
                best = s;
                bestTrack = ti;
            }
        }
        if (bestTrack >= 0 && best >= matchIouThreshold) {
            trackUsed[bestTrack] = true;
            detToTrack[idx] = bestTrack;
        }
    }

    for (int i = 0; i < static_cast<int>(results.size()); i++) {
        if (detToTrack[i] >= 0) continue;
        FaceTrack t;
        t.trackId = nextTrackId++;
        t.bbox = results[i].face;
        t.lastSeenMs = now;
        t.lastId = "";
        t.lastIdStreak = 0;
        t.stableId = "";
        t.stableConfidence = 0.0f;
        faceTracks.push_back(std::move(t));
        detToTrack[i] = static_cast<int>(faceTracks.size()) - 1;
    }

    std::vector<FaceTrack*> activeTracks;
    activeTracks.reserve(results.size());

    for (int i = 0; i < static_cast<int>(results.size()); i++) {
        const int ti = detToTrack[i];
        if (ti < 0 || ti >= static_cast<int>(faceTracks.size())) continue;
        FaceTrack& t = faceTracks[ti];
        t.bbox = results[i].face;
        t.lastSeenMs = now;

        const std::string rawId = results[i].identity.isAuthenticated ? results[i].identity.id : "Unknown";
        const float rawConf = results[i].identity.confidence;

        if (rawId == t.lastId) {
            t.lastIdStreak++;
        } else {
            t.lastId = rawId;
            t.lastIdStreak = 1;
        }

        if (t.lastIdStreak >= stableFrames) {
            t.stableId = rawId;
            t.stableConfidence = rawConf;
        } else if (t.stableId.empty()) {
            t.stableId = rawId;
            t.stableConfidence = rawConf;
        }

        activeTracks.push_back(&t);
    }

    if (!activeTracks.empty()) {
        std::sort(activeTracks.begin(), activeTracks.end(), [](const FaceTrack* a, const FaceTrack* b) {
            return a->trackId < b->trackId;
        });

        FaceTrack* bestAuth = nullptr;
        for (FaceTrack* t : activeTracks) {
            if (t->stableId == "Unknown") continue;
            if (!bestAuth || t->stableConfidence > bestAuth->stableConfidence) {
                bestAuth = t;
            }
        }

        for (FaceTrack* t : activeTracks) {
            const bool authed = (t->stableId != "Unknown");
            cv::Scalar color = authed ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
            int thickness = (bestAuth && t->trackId == bestAuth->trackId) ? 3 : 2;
            cv::rectangle(debugFrame, t->bbox, color, thickness);

            std::ostringstream label;
            label << "T" << t->trackId << " " << t->stableId;
            if (authed) {
                label << " " << static_cast<int>(t->stableConfidence * 100) << "%";
            }
            const cv::Point origin(std::max(0, t->bbox.x), std::max(0, t->bbox.y - 8));
            cv::putText(debugFrame, label.str(), origin, cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 2);
        }

        if (onResultCallback && (now - lastMultiMs > 650)) {
            lastMultiMs = now;
            std::ostringstream msg;
            msg << "FACES " << activeTracks.size();
            for (FaceTrack* t : activeTracks) {
                msg << " T" << t->trackId << "=" << t->stableId;
                if (t->stableId != "Unknown") {
                    msg << "(" << static_cast<int>(t->stableConfidence * 100) << "%)";
                }
            }
            onResultCallback(msg.str());
        }

        if (bestAuth) {
            if (onResultCallback && (now - lastVerifiedMs > 800)) {
                lastVerifiedMs = now;
                std::string msg = "VERIFIED " + bestAuth->stableId + " " + std::to_string(static_cast<int>(bestAuth->stableConfidence * 100)) + "%";
                onResultCallback(msg);
            }
        } else {
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
        renderFrameSeq++;
    }
}

bool Engine::getRenderFrame(cv::Mat& outFrame) {
    uint64_t seq = 0;
    return getRenderFrame(outFrame, seq);
}

bool Engine::getRenderFrame(cv::Mat& outFrame, uint64_t& outSeq) {
    std::lock_guard<std::mutex> lock(renderMutex);
    if (renderFrame.empty()) return false;
    renderFrame.copyTo(outFrame);
    outSeq = renderFrameSeq;
    return true;
}

void Engine::setFlip(bool flipX, bool flipY) {
    flipXEnabled.store(flipX);
    flipYEnabled.store(flipY);
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
