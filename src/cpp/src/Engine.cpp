/**
 * @file Engine.cpp
 * @brief Implementation of Engine class — 值流管线化编排器。
 */
#include "Engine.h"
#include "AccelerationContract.h"
#include "Config.h"
#include "Storage.h"
#include "NativeLog.h"
#include "rk_checked_arith.h"
#include "FaceAlign.h"    // 任务12：实时路径人脸对齐（关键点优先，无关键点回退 bbox 112×112）
#include "FaceSearch.h"   // 任务12：图库检索 searchTopK
#include "pipeline/FacePostProcessor.h"  // 任务12：实时检测框统一后处理（NMS/包含抑制）
#include <opencv2/imgproc.hpp>  // #1 注册采集：cvtColor 灰度化
#include <cctype>
#include <cmath>
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
// #1 注册采集：ArcFace/MobileFaceNet 模板维度，与 NativeFaceBridge.EMBEDDING_DIM 保持一致。
constexpr std::size_t kEmbeddingDim = 512;

// #1 标记本线程正身处注册采集回调内。回调同步重入 setEnrollmentCapture(false) 时据此
// 跳过「等待本线程在途回调」——自等必然死锁；跨线程关闭仍照常等待。
thread_local bool t_inEnrollmentCaptureCallback = false;

// ── FACE-PIPE-015：事件发射门「免死锁契约」的可执行化 ──
// 背景：renderResults 在**持有 identityEmitMu_** 的临界区内经 publisher_->publish 调用结果回调
//（Android 侧为 JNI 上行）。撤销路径 revokeTrackIdentitiesLocked 走 tracksMu_ → identityEmitMu_。
// 若回调**同步**回入任何取 tracksMu_ 的公开 API，即与此构成 ABBA 死锁。此前该约定仅存在于注释。
//
// 机制：以 thread_local 标记「本线程正处发射门内」，并让会取 tracksMu_ 的**全部 5 个公开入口**
//（run / processFrameOnce / setRealtimePipeline / applyGalleryUpdate / getTrackViews，穷尽集经审计枚举）
// 在入口处检测重入——命中即拒绝执行、返回明确的失败结果，绝不进入取锁流程。
// ⚠️ **边界（勿读成无条件保证）**：①**只有同一线程**的回入能被检测到——**跨线程**的同步回入（回调把工作
// 丢给另一线程并阻塞等待，如 `std::thread t(...); t.join();`）**绕开本机制**，仍构成真实 ABBA 死锁；
// ②标记是**进程级 thread_local**、非 per-Engine，同线程多 Engine 时会误拒另一个实例的调用。
// 详见 Engine.h 中 identityEmitMu_ 的完整条目。
// 该路径当前**生产不可达**（Android onNativeResult 仅 runOnUiThread 投递、不同步回入 Engine；
// src/win/ 未注册结果回调），故此举把「将来会死锁」变成「立刻得到明确错误」，对现有使用者零行为变化。
thread_local bool t_insideIdentityEmitGate = false;

// RAII：进入发射临界区即置位、退出**恢复原值**（异常/提前返回均安全）。
// 为什么保存/恢复而不是无条件置 false：标记是**进程级 thread_local**（非 per-Engine），一旦出现嵌套或
// 跨 Engine 的发射门，无条件清零会把外层尚未结束的标记提前抹掉。当前不可达（5 个入口已挡住重入的
// renderResults），但保存/恢复让它在未来守卫集收缩时不会变成锐边。（子代理4 指出）
class IdentityEmitGateScope {
public:
    IdentityEmitGateScope() : prev_(t_insideIdentityEmitGate) { t_insideIdentityEmitGate = true; }
    ~IdentityEmitGateScope() { t_insideIdentityEmitGate = prev_; }
    IdentityEmitGateScope(const IdentityEmitGateScope&) = delete;
    IdentityEmitGateScope& operator=(const IdentityEmitGateScope&) = delete;

private:
    const bool prev_;
};

// 取 tracksMu_ 的公开入口统一调用：命中「结果回调内同步重入」则记录固定 reason 并返回 true
//（调用方应据返回类型以 false / 空快照拒绝执行）。
bool reenteredFromIdentityEmitGate(const char* api) {
    if (!t_insideIdentityEmitGate) return false;
    rklog::logWarn("Engine", api,
                   "re-entered from result callback while holding identity emit gate");
    return true;
}

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

    // S-SEC-B09-003：32 位 checked 乘法——pixelStride 乘积溢出前拒绝；
    // 末字节用 int64 计算，32 位 size_t（armeabi-v7a）不再回绕导致越界读
    int yMaxCol = 0;
    if (!rk_jni::mulS32Checked(w - 1, yPixelStride, yMaxCol)) {
        err = "Y 平面 stride/pixelStride 乘积溢出";
        return false;
    }
    const int64_t yLast =
        static_cast<int64_t>(h - 1) * yRowStride +
        static_cast<int64_t>(w - 1) * yPixelStride;
    if (yRowStride <= yMaxCol || yLast >= static_cast<int64_t>(f.y.bytes.size())) {
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

    // S-SEC-B09-003：U/V pixelStride 为 JNI 侧 jint（无上限），(w2-1)*pixelStride
    // 可能 int32 溢出回绕为负导致检查失效；checked 乘法溢出前拒绝
    int uMaxCol = 0;
    if (!rk_jni::mulS32Checked(w2 - 1, uPixelStride, uMaxCol)) {
        err = "U 平面 stride/pixelStride 乘积溢出";
        return false;
    }
    const int64_t uLast =
        static_cast<int64_t>(h2 - 1) * uRowStride +
        static_cast<int64_t>(w2 - 1) * uPixelStride;
    if (uRowStride <= uMaxCol || uLast >= static_cast<int64_t>(f.u.bytes.size())) {
        err = "U 平面缓冲区不足或 stride 不匹配";
        return false;
    }
    int vMaxCol = 0;
    if (!rk_jni::mulS32Checked(w2 - 1, vPixelStride, vMaxCol)) {
        err = "V 平面 stride/pixelStride 乘积溢出";
        return false;
    }
    const int64_t vLast =
        static_cast<int64_t>(h2 - 1) * vRowStride +
        static_cast<int64_t>(w2 - 1) * vPixelStride;
    if (vRowStride <= vMaxCol || vLast >= static_cast<int64_t>(f.v.bytes.size())) {
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
    // NV21 色度子采样需偶数宽高：YUV2BGR_NV21 对奇数尺寸行为未定义/抛异常，显式拒绝
    if ((w & 1) != 0 || (h & 1) != 0) {
        err = "NV21 仅支持偶数宽高";
        return false;
    }

    const int stride = (f.nv21RowStrideY > 0) ? f.nv21RowStrideY : w;
    if (stride < w || stride <= 0) {
        err = "NV21 stride 非法";
        return false;
    }

    const int uvH = h / 2;
    // S-SEC-B09-003：int64 精确计算（32 位 size_t 平台不回绕），比较在 int64 域
    const int64_t needY = static_cast<int64_t>(stride) * h;
    const int64_t needUV = static_cast<int64_t>(stride) * uvH;
    if (needY + needUV > static_cast<int64_t>(f.nv21.size())) {
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

        const std::size_t uvSrcStart = static_cast<std::size_t>(needY);
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
    const uint8_t* srcVu = f.nv21.data() + static_cast<std::size_t>(needY);
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

// 限频闸门专用钟（单调）：**只**给 noteEmbedError 的窗口用。
// 本函数提供的就是 `pipeline::LogRateLimiter` 契约第 4 条要求的那个单调钟——限频器本身**不做**时钟
// 选择、也无法代为校验，时基由调用方（即 Engine 的 noteEmbedError）决定并传给它。
// ⚠️ 不可改用上面的 nowMs()（system_clock = 墙钟）：NTP / 手动校时**回拨**后 `now - last` 恒为负，
//    窗口永不满足 ⇒ 同一错误在时刻追平前一条之前**一条都不打**，与「禁止静默」（任务12）相反。
// TTL（Engine::trackFaces 传给 TrackCoordinator::update 的 nowMs()）仍用墙钟：它要的是可跨帧 /
// 跨会话比较的绝对时刻，语义与限频窗口不同，**不在本项射程**。
// ⚠️ 勿把 `setRealtimeReason` 也读成「用钟」：它**根本不看时钟**（判据只是 reason 文本是否变化，
//    见其实现）——此前提它与 TTL 并列为「用墙钟」是错的，已订正。
static long long steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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

#if defined(__ANDROID__)
    // 任务12：Android 实时路径默认走 RealtimeFacePipeline（SCRFD 检测 + ArcFace 识别），
    // 不再调用 BioAuth::verifyMulti（LBPH）。模型尚未加载时仅预览并显式 missing_model，
    // 绝不静默回退 LBPH（见 runRealtimeDetect）。宿主可在加载 manifest 后经
    // setRealtimePipeline 替换为带真实图库的管线。
    realtimePipeline_ = std::make_shared<RealtimeFacePipeline>();
    realtimeEnabled_.store(true);
#endif
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
    // 不在此提交 initialized_：由外层 initialize 成功后一次性 store(true)，
    // 避免 storage/bioAuth/videoManager 失败污染状态导致无法重试（Q-CQ-B06-006）
    RKLOG_ENTER("Engine");
    // 有意重置（Q-CQ-B06-006/007，仅记录未改行为）：initCommon 每次重新初始化清除
    // 既有取消意图，使取消后重试可正常执行。若调用方在 initialize() 之前
    // requestCancelInit()，此处会静默清除该意图——既有语义；JNI 入口也总是先
    // clearCancelInit() 再 initialize（src/cpp/jni/engine.cpp），改动取消语义有回归风险。
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
    std::lock_guard<std::mutex> lock(initMu_);
    if (initialized_.load()) {
        rklog::logWarn("Engine", __func__, "Engine 已经初始化，跳过重复调用");
        return true;
    }
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
    initialized_.store(true);
    return true;
}

bool Engine::initialize(const std::string& filePath, const std::string& cascadePath, const std::string& storagePath) {
    std::lock_guard<std::mutex> lock(initMu_);
    if (initialized_.load()) {
        rklog::logWarn("Engine", __func__, "Engine 已经初始化，跳过重复调用");
        return true;
    }
    if (!initCommon(cascadePath, storagePath)) return false;
    if (initCancelRequested_.load()) return false;
    if (filePath.empty()) {
        std::cerr << "Failed to open mock file: empty path" << std::endl;
        rklog::logError("Engine", __func__, "Failed to open mock file: empty path");
        return false;
    }
    if (!videoManager_->open(filePath)) {
        // S-SEC-B07-002：mock 文件路径可能是含 userinfo 的 URL，stderr 输出前脱敏
        std::cerr << "Failed to open mock file: " << VideoManager::sanitizeUriForLog(filePath) << std::endl;
        rklog::logError("Engine", __func__, "Failed to open mock file reason=" + videoManager_->getLastMockRejectReason());
        return false;
    }
    if (initCancelRequested_.load()) {
        videoManager_->close();
        return false;
    }
    performAccelSelfCheck();
    initialized_.store(true);
    return true;
}

void Engine::requestCancelInit() {
    initCancelRequested_.store(true);
}

void Engine::clearCancelInit() {
    initCancelRequested_.store(false);
}

void Engine::run() {
    // FACE-PIPE-015：run() 是本类的**阻塞主循环**，但同为「会取 tracksMu_ 的公开入口」——它经
    // processFrame → processFrameRealtime 取 tracksMu_，随后 renderResults 会在**同线程**再取
    // identityEmitMu_（非递归）。故若被结果回调（已持 identityEmitMu_）同步调用，即构成自锁。
    // 与 applyGalleryUpdate / setRealtimePipeline / getTrackViews / processFrameOnce 一致地拒绝：
    // 直接返回、不进入循环（A1 审计指出此前漏了本入口，与注释宣称的「所有」不符）。
    if (reenteredFromIdentityEmitGate(__func__)) return;
    RKLOG_ENTER("Engine");
    isRunning_ = true;
    cv::Mat frame;
    // 连续异常退避（本 run() 生命周期）：超过阈值后短睡抑制错误日志风暴（Q-CQ-B06-007）
    int consecutiveErrors = 0;
    constexpr int kErrorBackoffThreshold = 50;
    constexpr int kErrorBackoffSleepMs = 100;
    // 仅在 catch 中调用；首次越过阈值打一次提示日志，之后每次异常短睡退避。
    // 计数器在 try 体正常走完后清零（见下方 consecutiveErrors = 0），异常恢复即退出退避。
    // MSVC 对 constexpr 局部变量在 lambda 内隐式捕获较严格，用 [&] 引用捕获全部所需变量
    auto backoffOnError = [&]() {
        if (++consecutiveErrors >= kErrorBackoffThreshold) {
            if (consecutiveErrors == kErrorBackoffThreshold) {
                rklog::logWarn("Engine", "run",
                    "连续异常退避: 连续 " + std::to_string(kErrorBackoffThreshold) +
                    " 次异常，此后每次异常间 sleep " + std::to_string(kErrorBackoffSleepMs) + "ms");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kErrorBackoffSleepMs));
        }
    };

    std::cout << "Engine started." << std::endl;

    while (isRunning_) {
        try {
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
            // 注意：clone 保护 cv::putText 不会修改共享帧数据
            if (currentMode_ == MonitoringMode::MOTION_TRIGGERED &&
                !motionDetector_->detect(frame)) {
                pipeline::FrameOutcome outcome;
                outcome.renderFrame = frame.clone();
                cv::putText(outcome.renderFrame, "WAITING", cv::Point(20, 40),
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);
                publisher_->publish(std::move(outcome));
                continue;
            }

            // 3-7. 委托 processFrame 管线
            processFrame(frame, decodeMs);
            // 正常走完一轮视为异常已恢复，清零连续异常计数（continue 路径不计数也不清零，无影响）
            consecutiveErrors = 0;
        } catch (const std::exception& e) {
            rklog::logError("Engine", __func__, "处理循环异常被捕获: " + std::string(e.what()));
            backoffOnError();
        } catch (...) {
            rklog::logError("Engine", __func__, "处理循环未知异常被捕获");
            backoffOnError();
        }
    }
}

bool Engine::processFrame(const cv::Mat& rawFrame, double decodeMs) {
    // 3. 预处理：resize + 统一变换（FrameTransform::applyImage，仅此一次）。
    // 变换后的 processedFrame_ 同时作为「推理输入」与「渲染输出」，检测框天然处于
    // 同一坐标系，无需二次映射（任务1 统一变换语义）。
    if (!preprocessFrame(rawFrame)) return false;

    // 4. 分支：启用实时路径后（Android 默认 / 已注入管线），检测/识别/跟踪由
    //    RealtimeFacePipeline 产出；否则保持 BioAuth::verifyMulti 兼容路径。
    //    关键：分支依据是「是否启用实时模式」而非「是否有可用快照」——模型缺失时
    //    仍留在实时路径并显式 missing_model，绝不在运行时静默回退 LBPH（任务12 约束）。
    if (realtimeEnabled_.load()) {
        return processFrameRealtime(decodeMs);
    }

    // 兼容路径：检测 + 识别（委托 BioAuth，LBPH）
    std::vector<BioAuth::FaceAuthResult> results;
    bioAuth_->verifyMulti(processedFrame_, results, 4, true);

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
    // 兼容路径每帧都实际执行了 BioAuth 检测，故 detectionRan=true（任务5 消失规则生效）
    // 兼容路径不经图库覆盖匹配，传 kNoGalleryGuard 表示不做图库代次作废。
    trackFaces(faces, true, kNoGalleryGuard);

    // 6-7. 渲染 + 性能统计
    currentStats_.decodeMs = decodeMs;
    renderResults();
    collectStats();
    return true;
}

bool Engine::setRealtimePipeline(std::shared_ptr<RealtimeFacePipeline> pipeline) {
    // FACE-PIPE-015：本 API 会取 tracksMu_。若从结果回调（持发射门）同步重入，将与撤销路径构成
    // ABBA 死锁；入口检测命中即拒绝执行（返回 false + 固定 reason），绝不进入取锁流程。
    if (reenteredFromIdentityEmitGate(__func__)) return false;
    bool enabled = false;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(realtimeMu_);
        // 幂等判定（第四轮审查 M1）：按**指向对象**比较新旧 shared_ptr（get()），而非控制块。
        // 同对象重复注入（宿主配置刷新/重复激活）不构成「换模型」，据此跳过下表的重置与撤销，
        // 避免在途稳定身份被抹、需重新累计 stableFrames、画面短暂回退 Unknown，并使紧随其后的
        // injectGalleryToEngine(keepIds) 的精准撤销无物可保。启停（null ↔ 非 null）与换到新对象
        // 均视为变化，仍走完整撤销——「换模型必撤销」语义不变。
        changed = (realtimePipeline_.get() != pipeline.get());
        realtimePipeline_ = std::move(pipeline);
        enabled = (realtimePipeline_ != nullptr);
    }
    realtimeEnabled_.store(enabled);
    if (changed) {
        // 仅在管线真正变化时执行。锁序固定 tracksMu_ → realtimeGalleryMu_（与 applyGalleryUpdate
        // 一致，避免形成环）。
        std::lock_guard<std::mutex> trackLock(tracksMu_);
        {
            // 重置图库覆盖：新管线经 loadGallery 钩子提供自身图库，旧覆盖必须丢弃，
            // 否则换管线后会继续用上一套图库做身份匹配（陈旧身份）。
            // 一并递增图库代次：使在途帧据旧覆盖得出的认证结论在提交点作废（见 trackFaces）。
            std::lock_guard<std::mutex> lock(realtimeGalleryMu_);
            realtimeGallery_.reset();
            galleryGeneration_.fetch_add(1, std::memory_order_release);
        }
        // 换管线 = 换模型：新旧模型嵌入空间不同，旧模型下得出的 stableId 一律失效（即便新库
        // 含同一 personId 也不可沿用——其模板处于不同嵌入空间）。故与 applyGalleryUpdate 同语义
        // 撤销全部跟踪身份（保留轨迹本体与 bbox），并递增 identityVersion_ 使在途渲染在发布点
        // 作废旧身份结果（否则换模型后残留旧 id 会持续输出 VERIFIED）。
        revokeTrackIdentitiesLocked({});
    }
    if (!enabled) {
        // 切回兼容路径时清空实时 reason，避免状态区展示过期原因
        std::lock_guard<std::mutex> lock(realtimeReasonMu_);
        realtimeReason_.clear();
    }
    rklog::logInfo("Engine", __func__,
                   std::string("实时推理管线") + (enabled ? "已注入/启用" : "已清除，回退兼容路径") +
                   (changed ? "" : "（同对象重复注入，幂等跳过撤销）"));
    return true;
}

uint64_t Engine::getRealtimeGeneration() const {
    std::shared_ptr<RealtimeFacePipeline> pipe;
    {
        std::lock_guard<std::mutex> lock(realtimeMu_);
        pipe = realtimePipeline_;
    }
    return pipe ? pipe->generation() : 0;
}

std::string Engine::getRealtimeReason() const {
    std::lock_guard<std::mutex> lock(realtimeReasonMu_);
    return realtimeReason_;
}

void Engine::setRealtimeReason(const std::string& reason) {
    std::string prev;
    {
        std::lock_guard<std::mutex> lock(realtimeReasonMu_);
        if (realtimeReason_ == reason) return;  // 未变化则不重复记录（避免逐帧日志风暴）
        prev = realtimeReason_;
        realtimeReason_ = reason;
    }
    // 仅在状态变化时记录：模型缺失等失败 reason 显式可观测（任务12 禁止静默）。
    if (!reason.empty() && reason != "ok") {
        rklog::logWarn("Engine", __func__, "实时路径 reason=" + reason);
    } else if (reason == "ok" && !prev.empty()) {
        rklog::logInfo("Engine", __func__, "实时路径恢复 ok（此前 reason=" + prev + "）");
    }
}

// 逐帧 × 每脸路径上的 embed / 图库检索失败：把原本被丢弃的差异化 err 记为**一条**可观测日志，
// 同时限频——这是「禁止静默」（任务12）与「避免日志风暴」的折中。
// ⚠️ 与 setRealtimeReason 的「值变化才记」范式**不同**，勿照搬：那条范式对「多个不同值交替出现」
// 没有任何上界（每次都是「值变了」）。
// **刻意不复用 realtimeReason_**：那是对外 reason 契约通道（getRealtimeReason / getRuntimeStatus /
// native_face_bridge 读取），把逐脸错误写进去会污染其语义。err 文本**只进日志**，不外传 Java、
// 不进 getRuntimeStatus。
void Engine::noteEmbedError(const char* site, const std::string& err) {
    if (!site || err.empty()) return;  // 空 err 无诊断价值，不记（避免与「无错误」混淆）
    {
        std::lock_guard<std::mutex> lock(embedErrMu_);
        // 限频：唯一判据是时间 ⇒ 日志量有全局上界（每窗最多一条）。判据与窗口推进同锁，
        // 否则两个线程会各自读到未推进的起点而同时放行。理由与反例见 LogRateLimiter.h 的契约。
        if (!embedErrLimiter_.tryAcquire(steadyNowMs())) return;
    }
    // 锁外记录：不持锁做 I/O。
    rklog::logWarn("Engine", site, "人脸特征路径失败 err=" + err);
}

bool Engine::applyGalleryUpdate(std::shared_ptr<const FaceSearchLinearIndex> gallery,
                                const std::vector<std::string>& keepIds) {
    // FACE-PIPE-015：本 API 会取 tracksMu_（并在其内等待 identityEmitMu_）。若从结果回调（持发射门）
    // 同步重入，将与撤销路径构成 ABBA 死锁；入口检测命中即拒绝执行（返回 false + 固定 reason），绝不
    // 进入取锁流程。该路径当前生产不可达，故对现有使用者零行为变化。
    if (reenteredFromIdentityEmitGate(__func__)) return false;
    // 第三波审查 #4 残留竞态闭合：把「换图库覆盖」与「按新库有效身份精准撤销跟踪身份」放进
    // **同一个 tracksMu_ 临界区**，使引擎线程的 trackFaces/update 无法插入二者之间——它要么在
    // 换库前完成（撤销随后覆盖其结果），要么在换库后完成（据新库处理），绝不出现「已撤身份又
    // 被旧库结论重新写入」的中间态。
    // 锁序固定 tracksMu_ → realtimeGalleryMu_；assign 侧仅单独持 realtimeGalleryMu_（不嵌套
    // tracksMu_），trackFaces 仅持 tracksMu_，故不构成环，无死锁。
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> trackLock(tracksMu_);
        {
            std::lock_guard<std::mutex> galLock(realtimeGalleryMu_);
            realtimeGallery_ = std::move(gallery);
            // 换库与代次递增同锁，保证 assign 侧读到的 (gallery, generation) 一致。
            galleryGeneration_.fetch_add(1, std::memory_order_release);
            count = realtimeGallery_ ? realtimeGallery_->size() : 0;
        }
        // 与换库同一临界区：撤销 coordinator 内部 tracks_（身份真相）与对外 currentTracks_ 快照。
        revokeTrackIdentitiesLocked(keepIds);
    }
    // 仅记录条目数与有效身份数，绝不含身份/模板内容（安全红线）。
    rklog::logInfo("Engine", __func__,
                   "实时图库覆盖已更新并精准撤销身份 count=" + std::to_string(count) +
                   " keep=" + std::to_string(keepIds.size()));
    return true;
}

void Engine::revokeTrackIdentitiesLocked(const std::vector<std::string>& keepIds) {
    // 调用方须已持 tracksMu_。trackCoordinator_ 是身份真相持有者；currentTracks_ 是逐帧派生的
    // 对外快照，二者都要按有效身份撤销，否则对外视图会残留旧身份直到下一帧。
    if (trackCoordinator_) trackCoordinator_->resetIdentitiesExcept(keepIds);
    for (auto& v : currentTracks_) {
        // 与 coordinator 同规则：保留 keepIds 中的身份，其余清为 Unknown；保留 trackId/bbox
        //（不清空 currentTracks_——否则多人场景会连同其他人的框一起丢）。
        if (v.stableId == "Unknown") continue;
        bool keep = false;
        for (const auto& id : keepIds) {
            if (v.stableId == id) { keep = true; break; }
        }
        if (!keep) {
            v.stableId = "Unknown";
            v.stableConfidence = 0.0f;
        }
    }
    // 身份已（可能）变更：递增身份版本号，使在途 renderResults 在发布点作废旧快照身份
    //（发布路径 P1）。调用方持 tracksMu_，与本函数内所有身份改动同临界区，保证版本与身份
    // 变更对外原子可见。仅在确有对外撤销入口调用时递增（applyGalleryUpdate /
    // setRealtimePipeline），trackFaces 的正常逐帧提交不递增。
    identityVersion_.fetch_add(1, std::memory_order_release);
    // 与事件发射握手（第五轮审查 P1）：先递增版本（使**尚未进入**发射临界区的在途帧在最终校验时
    // 必然看见新版本并清身份），再取发射门、等待**已经进入**发射的帧结束才返回。二者合起来才保证
    // 「本函数返回后不会再发出已撤销身份的 VERIFIED」——只递增不等、或只等不递增，都会留下窗口。
    // 本函数已持 tracksMu_；锁序 tracksMu_ → identityEmitMu_ 与 renderResults 一致（后者取发射门
    // 之前已释放 tracksMu_），故无环。等待期间引擎线程正在 publish 内，其不再取 tracksMu_，必定能完成。
    {
        std::lock_guard<std::mutex> emitGate(identityEmitMu_);
        (void)emitGate;  // 仅以「取得并释放」表达与在途发射的互斥/等待语义
    }
}

// ── #1 注册采集模式 ──

void Engine::setEnrollmentCapture(bool enabled) {
    // 持锁切换 enabled；关闭时等待在途回调归零，保证返回即无跨线程在途回调，
    // JNI 侧据此安全释放回调中使用的全局引用/缓冲区。
    std::unique_lock<std::mutex> lock(enrollmentCaptureMu_);
    enrollmentCaptureEnabled_.store(enabled);
    if (enabled) return;
    if (t_inEnrollmentCaptureCallback) {
        // 本线程正身处回调内（回调同步重入关闭）：不能等待自身，否则死锁。
        // 该回调返回后不会再触达已释放资源，故直接放行。
        return;
    }
    enrollmentCaptureCv_.wait(lock, [this] { return enrollmentCaptureInFlight_ == 0; });
}

void Engine::setEnrollmentCaptureCallback(EnrollmentCaptureCallback callback) {
    std::lock_guard<std::mutex> lock(enrollmentCaptureMu_);
    enrollmentCaptureCb_ = std::move(callback);
}

bool Engine::computeEnrollmentQuality(const cv::Mat& faceBgr, float& grayMean,
                                      float& grayStd, float& laplacianVariance) {
    grayMean = 0.0f;
    grayStd = 0.0f;
    laplacianVariance = 0.0f;
    if (faceBgr.empty()) return false;

    cv::Mat gray;
    if (faceBgr.channels() == 3) {
        cv::cvtColor(faceBgr, gray, cv::COLOR_BGR2GRAY);
    } else if (faceBgr.channels() == 4) {
        cv::cvtColor(faceBgr, gray, cv::COLOR_BGRA2GRAY);
    } else if (faceBgr.channels() == 1) {
        gray = faceBgr;
    } else {
        return false;
    }
    if (gray.empty() || gray.type() != CV_8U) return false;
    if (!gray.isContinuous()) gray = gray.clone();

    const int w = gray.cols;
    const int h = gray.rows;
    if (w <= 0 || h <= 0) return false;

    // 灰度总体统计（与 EnrollmentSession.ImageMetrics 一致：按无符号字节解释）。
    double sum = 0.0;
    double sumSq = 0.0;
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* row = gray.ptr<std::uint8_t>(y);
        for (int x = 0; x < w; ++x) {
            const double v = static_cast<double>(row[x]);
            sum += v;
            sumSq += v * v;
        }
    }
    const double n = static_cast<double>(w) * static_cast<double>(h);
    const double mean = sum / n;
    grayMean = static_cast<float>(mean);
    grayStd = static_cast<float>(std::sqrt(std::max(0.0, sumSq / n - mean * mean)));

    // Laplacian 4 邻域（[0 1 0; 1 -4 1; 0 1 0]）响应方差：仅内部像素，
    // 公式与 EnrollmentSession.ImageMetrics.laplacianVariance 逐项一致，确保阈值语义对齐。
    if (w < 3 || h < 3) {
        laplacianVariance = 0.0f;
        return true;
    }
    double lsum = 0.0;
    double lsumSq = 0.0;
    long long count = 0;
    for (int y = 1; y < h - 1; ++y) {
        const std::uint8_t* up = gray.ptr<std::uint8_t>(y - 1);
        const std::uint8_t* mid = gray.ptr<std::uint8_t>(y);
        const std::uint8_t* down = gray.ptr<std::uint8_t>(y + 1);
        for (int x = 1; x < w - 1; ++x) {
            const int lap = 4 * static_cast<int>(mid[x]) - static_cast<int>(mid[x - 1]) -
                            static_cast<int>(mid[x + 1]) - static_cast<int>(up[x]) -
                            static_cast<int>(down[x]);
            lsum += static_cast<double>(lap);
            lsumSq += static_cast<double>(lap) * static_cast<double>(lap);
            ++count;
        }
    }
    if (count == 0) {
        laplacianVariance = 0.0f;
        return true;
    }
    const double lmean = lsum / static_cast<double>(count);
    laplacianVariance =
        static_cast<float>(std::max(0.0, lsumSq / static_cast<double>(count) - lmean * lmean));
    return true;
}

void Engine::captureEnrollmentFaces(const cv::Mat& frame,
                                    const std::vector<pipeline::DetectedFace>& faces,
                                    const RealtimePipelineSnapshot& snap) {
    if (!snap.embedder || frame.empty() || faces.empty()) return;

    // 采集模型与匹配共用同一 embedder；输入尺寸取 profile 契约（默认 112）。
    FaceAlignOptions alignOpt;
    alignOpt.outW = snap.profile.embedderInputW > 0 ? snap.profile.embedderInputW : 112;
    alignOpt.outH = snap.profile.embedderInputH > 0 ? snap.profile.embedderInputH : 112;

    const int faceCount = static_cast<int>(faces.size());

    for (const pipeline::DetectedFace& df : faces) {
        FaceDetection det;
        det.bbox = cv::Rect2f(static_cast<float>(df.bbox.x), static_cast<float>(df.bbox.y),
                              static_cast<float>(df.bbox.width),
                              static_cast<float>(df.bbox.height));
        det.score = df.confidence;
        // 有 5 点关键点时优先关键点仿射对齐；否则 bbox fallback（与身份匹配同策略）。
        if (df.hasKeypoints) {
            std::array<cv::Point2f, 5> kp;
            for (std::size_t i = 0; i < 5; ++i) {
                kp[i] = cv::Point2f(df.keypoints[i * 2], df.keypoints[i * 2 + 1]);
            }
            det.keypoints5 = kp;
        }
        const FaceAlignResult ar = alignFaceForArcFace112(frame, det, alignOpt);
        if (ar.alignedBgr.empty()) {
            // 对齐失败早退（逐脸路径**第 5 处**丢弃，此前只有裸 continue ⇒ 静默）。
            // 无向量可清（对齐阶段不产出特征向量），只补记一条限频日志——与下方 ①②③ 共用同一
            // noteEmbedError 时间窗（**不新增上界**）。err 必非空，理由见下方 ④。
            noteEmbedError("captureEnrollmentFaces/align", ar.err);
            continue;
        }

        std::vector<float> emb;
        std::string eerr;
        if (!snap.embedder->embed(ar.alignedBgr, emb, eerr)) {
            // 失败也可能已写入部分明文，丢弃前清零（模板红线）。**四条口径，六个站点，均不静默**：
            // ① 本处（embed 失败）与 assignRealtimeIdentities 的 embed 失败支——IRealtimeEmbedder::
            //    embed 的契约**未承诺**失败时不动 out（生产包装 EmbedderRealtimeAdapter 确实不写，
            //    但注入式 embedder 可以写），故两处都以「失败即清零 + noteEmbedError」为准，不依赖
            //    某个具体包装实现；
            // ② 紧跟其后的「维度不符」支——同样清零，并补记 noteEmbedError（此前该支只有
            //    std::fill + continue，是逐脸路径上第 4 处**静默**丢弃，本批补齐）；
            // ③ 识别侧的图库检索失败（assignRealtimeIdentities 的 search 失败支）——无向量可清，
            //    只记 noteEmbedError；
            // ④ **两处对齐失败**——本函数与 assignRealtimeIdentities 各自的 `ar.alignedBgr.empty()`
            //    早退（逐脸路径上第 5、6 处**静默**丢弃，本批补齐）：同样无向量可清（对齐阶段不产出
            //    特征向量），只记 noteEmbedError，err 文本取自 `FaceAlignResult::err`
            //    （FaceAlign.cpp：「输入图像为空」/「输出尺寸非法」/「bbox 越界或为空」/
            //    「bbox 裁剪/resize 失败」——凡返回空 alignedBgr 的出口都设了非空 err，故必记成一条）。
            // ①②③④ 全部走同一个 noteEmbedError ⇒ **共用同一时间窗限频器**：站点从 4 个增至 6 个
            // **不新增任何上界**（窗内最多一条，与 site 个数无关）。
            std::fill(emb.begin(), emb.end(), 0.0f);
            noteEmbedError("captureEnrollmentFaces/embed", eerr);
            continue;
        }
        // 维度不符（如 profile 与 embedder 漂移）时拒绝该脸，避免向 Java 喂入非法样本；
        // 该向量同样可能含部分明文，丢弃前清零。
        // ⚠️ 此处**不是**静默丢弃：err 文本带实际 / 期望维度，供排障判定「profile 与 embedder 漂移」
        //    还是「模型输出维度本身不对」。err 文本只含维度数字，不含模板数据，仅落本地日志。
        if (emb.size() != static_cast<std::size_t>(kEmbeddingDim)) {
            std::fill(emb.begin(), emb.end(), 0.0f);
            noteEmbedError("captureEnrollmentFaces/dim",
                           "维度不符: 实际 " + std::to_string(emb.size()) + " 期望 " +
                               std::to_string(static_cast<std::size_t>(kEmbeddingDim)));
            continue;
        }

        EnrollmentCaptureSample sample;
        sample.embedding = std::move(emb);
        sample.bbox = df.bbox;
        sample.frameWidth = frame.cols;
        sample.frameHeight = frame.rows;
        sample.detScore = df.confidence;
        sample.faceCount = faceCount;
        sample.hasLandmarks = df.hasKeypoints;
        sample.landmarks = df.keypoints;
        float gm = 0.0f;
        float gs = 0.0f;
        float lv = 0.0f;
        if (computeEnrollmentQuality(ar.alignedBgr, gm, gs, lv)) {
            sample.grayMean = gm;
            sample.grayStd = gs;
            sample.laplacianVariance = lv;
        }

        // 锁内复制回调快照并登记在途，锁外调用：回调同步重入 stop/start 时不会在同线程
        // 二次加锁自死锁（参考 ResultPublisher::publish 的 cbMu_ 快照模式）。关闭路径经
        // enrollmentCaptureCv_ 等待在途计数归零，故跨线程关闭返回即无在途回调。
        EnrollmentCaptureCallback cb;
        {
            std::lock_guard<std::mutex> lock(enrollmentCaptureMu_);
            if (enrollmentCaptureEnabled_.load() && enrollmentCaptureCb_) {
                cb = enrollmentCaptureCb_;
                ++enrollmentCaptureInFlight_;
            }
        }
        if (cb) {
            t_inEnrollmentCaptureCallback = true;
            try {
                cb(sample);
            } catch (...) {
                // 回调异常不得穿透实时帧循环，也不得破坏在途计数：吞掉后照常收尾。
            }
            t_inEnrollmentCaptureCallback = false;
            {
                std::lock_guard<std::mutex> lock(enrollmentCaptureMu_);
                if (--enrollmentCaptureInFlight_ == 0) enrollmentCaptureCv_.notify_all();
            }
        }
        // 模板红线：局部队列用毕立即清零，不留明文。
        std::fill(sample.embedding.begin(), sample.embedding.end(), 0.0f);
    }
}

bool Engine::getRealtimeEffective() const {
    std::shared_ptr<RealtimeFacePipeline> pipe;
    {
        std::lock_guard<std::mutex> lock(realtimeMu_);
        pipe = realtimePipeline_;
    }
    if (!pipe) return false;
    const std::shared_ptr<const RealtimePipelineSnapshot> snap = pipe->snapshot();
    return snap && snap->effective;
}

std::vector<pipeline::TrackView> Engine::getTrackViews() const {
    // FACE-PIPE-015：本 API 会取 tracksMu_。若从结果回调（持发射门）同步重入，将并入 ABBA 死锁环；
    // 入口检测命中即返回空快照（明确的失败结果），不取锁。
    if (reenteredFromIdentityEmitGate(__func__)) return {};
    std::lock_guard<std::mutex> lock(tracksMu_);
    return currentTracks_;
}

bool Engine::processFrameOnce(const cv::Mat& rawFrame, double decodeMs) {
    // FACE-PIPE-015：本 API 经 trackFaces/renderResults 会取 tracksMu_。若从结果回调（持发射门）
    // 同步重入（属误用——回调本应只投递），将与撤销路径构成 ABBA 死锁；入口检测命中即拒绝执行。
    if (reenteredFromIdentityEmitGate(__func__)) return false;
    return processFrame(rawFrame, decodeMs);
}

bool Engine::processFrameRealtime(double decodeMs) {
    std::vector<pipeline::DetectedFace> faces;
    bool detectionRan = false;
    if (runRealtimeDetect(processedFrame_, faces, detectionRan)) {
        // 传入本帧读库所用代次：若换库发生在读库与提交之间，trackFaces 会作废本帧认证结论。
        trackFaces(faces, detectionRan, frameGalleryGeneration_);
    } else {
        // 模型/管线不可用：继续预览（发布当前帧）但显式 reason（missing_model 等），
        // 清空对外轨迹以避免展示过期身份；不调用 BioAuth（禁止回退），也不新建/删除轨迹。
        std::lock_guard<std::mutex> lock(tracksMu_);
        currentTracks_.clear();
    }
    currentStats_.decodeMs = decodeMs;
    renderResults();
    collectStats();
    return true;
}

bool Engine::runRealtimeDetect(const cv::Mat& frame,
                               std::vector<pipeline::DetectedFace>& outFaces,
                               bool& detectionRan) {
    outFaces.clear();
    detectionRan = false;

    std::shared_ptr<RealtimeFacePipeline> pipe;
    {
        std::lock_guard<std::mutex> lock(realtimeMu_);
        pipe = realtimePipeline_;
    }
    if (!pipe) {
        setRealtimeReason("runtime_init_failed");
        return false;
    }

    // 快照不可用（模型未加载/装配失败）→ 显式 reason，继续预览，不进入 BioAuth。
    const std::shared_ptr<const RealtimePipelineSnapshot> snap = pipe->snapshot();
    if (!snap || !snap->effective || !snap->detector) {
        setRealtimeReason((snap && !snap->reason.empty()) ? snap->reason : std::string("missing_model"));
        return false;
    }

    std::vector<RealtimeFaceBox> boxes;
    std::string err;
    if (!pipe->detect(frame, boxes, err)) {
        setRealtimeReason(err.empty() ? "runtime_init_failed" : err);
        return false;
    }

    // 真实检测已执行：即使 boxes 为空也置 detectionRan=true，使「实际空检测」按
    // 任务5 规则让旧框在连续缺失 2 次后消失（区别于模型缺失的跳过语义）。
    detectionRan = true;
    outFaces = postProcessRealtimeBoxes(frame, boxes, snap->profile.nmsIou);
    // #1 注册采集：仅采集模式开启时逐脸产出样本（关闭时快速分支零开销）。
    // 必须在身份匹配之前调用——assignRealtimeIdentities 命中时会把 confidence 覆盖为
    // 相似度，而采集样本的 detScore 必须是原始检测分。身份匹配与采集相互独立，无顺序依赖。
    if (enrollmentCaptureEnabled_.load()) {
        captureEnrollmentFaces(frame, outFaces, *snap);
    }
    assignRealtimeIdentities(frame, outFaces, *snap);
    setRealtimeReason("ok");
    return true;
}

std::vector<pipeline::DetectedFace> Engine::postProcessRealtimeBoxes(
    const cv::Mat& frame, const std::vector<RealtimeFaceBox>& boxes, float nmsIouThreshold) {
    std::vector<pipeline::DetectedFace> raw;
    raw.reserve(boxes.size());
    for (const RealtimeFaceBox& b : boxes) {
        // 丢弃 NaN/Inf：整数化前显式校验，避免 float→int 未定义行为（FacePostProcessor
        // 的 NaN 检查针对 confidence，此处负责几何字段）。
        if (!std::isfinite(b.x) || !std::isfinite(b.y) ||
            !std::isfinite(b.width) || !std::isfinite(b.height)) {
            continue;
        }
        // ★ 2026-09-28 修（外部审查 [P2]「异常检测框可触发整数转换或加法溢出」+ 独立复核）：
        //   原实现只挡 NaN/Inf，随后**直接**把 float 转 int。按 C++ `[conv.fpint]`，截断值**不可表示**
        //   即 **UB**；而 SCRFD 的 distance/delta 回归分量**无上界**（畸形 / 敌意模型可给出 ~1e9 量级的
        //   **有限**值）⇒ 这条 UB 是真实可达的，不只是理论问题。
        //   修法：**先约束、后转换**（与 `FaceAlign.cpp` 的 `clampRectToImage` 同一范式）：
        //     ① 帧非空时，端点各自夹进 `[0, frame.cols]` / `[0, frame.rows]`（全程 double，不溢出）；
        //     ② 帧为空（cols/rows <= 0）时**不夹进帧边界**，以对齐 `FacePostProcessor::keptIndices`
        //        的 `clipToFrame == false` 语义（那里同样不裁剪）—— 但**仍**把端点夹进
        //        `±INT_MAX/2`，因为下面要把端点差转成 int，而「端点各自在 int 域内」**不足以**
        //        保证差值也在（如 ±2e9 相减）；限制在半个 int 域即让差值必然可表示。
        //   ⚠️ 该分支的保证**只有「无 UB」这一条，不含「框不被丢」**（对抗性代理 A2 给出反例）：
        //      若两端点都落在 ±INT_MAX/2 之外，夹取后二者**同值** ⇒ 下面 `x2d - x1d == 0` ⇒
        //      宽度 0，而下游 `keptIndices`（`FacePostProcessor.cpp` 的 `f.bbox.width <= 0 → continue`）
        //      会**丢弃**该框 ⇒ 这一支的框数是**会变**的，不要据本注释断言「不丢框」。
        //   ⚠️ 保持既有语义：仍是**半开区间**（下界 floor、上界 ceil），**不要**改成 `lround`
        //      —— 那会破坏既有注释固化的「避免边缘框 1px 漂移」约定。
        constexpr double kIntHalf = 1073741823.0;  // = INT_MAX / 2（写成字面量：免引 <limits> 到本 TU）
        double x1d = std::floor(static_cast<double>(b.x));
        double y1d = std::floor(static_cast<double>(b.y));
        double x2d = std::ceil(static_cast<double>(b.x) + static_cast<double>(b.width));
        double y2d = std::ceil(static_cast<double>(b.y) + static_cast<double>(b.height));
        if (frame.cols > 0 && frame.rows > 0) {
            const double maxX = static_cast<double>(frame.cols);
            const double maxY = static_cast<double>(frame.rows);
            x1d = std::min(std::max(x1d, 0.0), maxX);
            y1d = std::min(std::max(y1d, 0.0), maxY);
            x2d = std::min(std::max(x2d, 0.0), maxX);
            y2d = std::min(std::max(y2d, 0.0), maxY);
        } else {
            x1d = std::min(std::max(x1d, -kIntHalf), kIntHalf);
            y1d = std::min(std::max(y1d, -kIntHalf), kIntHalf);
            x2d = std::min(std::max(x2d, -kIntHalf), kIntHalf);
            y2d = std::min(std::max(y2d, -kIntHalf), kIntHalf);
        }
        pipeline::DetectedFace df;
        df.bbox = cv::Rect(static_cast<int>(x1d), static_cast<int>(y1d),
                           std::max(0, static_cast<int>(x2d - x1d)),
                           std::max(0, static_cast<int>(y2d - y1d)));
        df.confidence = b.score;  // 检测置信度；身份命中后覆盖为相似度
        df.identityId = "Unknown";
        df.isAuthenticated = false;
        // #1 注册采集：把检测关键点透传到后处理结果（NMS/包含抑制按分数保留的框携带自身关键点）。
        df.hasKeypoints = b.hasKeypoints;
        df.keypoints = b.keypoints;
        raw.push_back(std::move(df));
    }

    pipeline::FacePostProcessOptions opt;
    opt.nmsIouThreshold = nmsIouThreshold;
    // 关键：IoU NMS + 包含框抑制，确保同一张脸的局部/嵌套框只保留一个，
    // 避免下游 TrackCoordinator 因多个框各建轨迹而产生「第二身份」。
    return pipeline::FacePostProcessor::process(std::move(raw), frame.cols, frame.rows, opt);
}

void Engine::assignRealtimeIdentities(const cv::Mat& frame,
                                      std::vector<pipeline::DetectedFace>& faces,
                                      const RealtimePipelineSnapshot& snap) {
    // 图库来源：优先运行时覆盖快照（applyGalleryUpdate 在 Engine 侧重建并替换的快照 gallery
    // 字段），否则用快照自带 gallery。
    std::shared_ptr<const FaceSearchLinearIndex> gallery;
    {
        std::lock_guard<std::mutex> lock(realtimeGalleryMu_);
        gallery = realtimeGallery_;
        // 记录本帧读库所用代次：trackFaces 提交前在 tracksMu_ 内比对，若图库在「读库」与
        // 「提交轨迹」之间被替换（applyGalleryUpdate），则本帧据旧库得出的认证结论作废
        //（#4 残留竞态闭合）。与 gallery 同锁读取，保证 (gallery, generation) 一致。
        frameGalleryGeneration_ = galleryGeneration_.load(std::memory_order_acquire);
    }
    if (!gallery) gallery = snap.gallery;

    // 身份识别为增强项：空图库/无 embedder 合法，身份保持 Unknown（这是实时路径的
    // 正常空结果，不是 LBPH 回退）。
    if (!snap.embedder || !gallery || gallery->size() == 0) return;

    // #1 对齐一致性：身份匹配与注册采集必须走同一对齐策略。DetectedFace 现已携带
    // 关键点（hasKeypoints/keypoints），故此处与 captureEnrollmentFaces 完全对齐：
    // 有 5 点关键点时优先关键点仿射对齐，无关键点时才回退 bbox。否则注册模板与查库
    // probe 来自不同对齐，同一人脸 cosine 可能跌破 identitySimilarity → 注册后识别不到。
    FaceAlignOptions alignOpt;
    alignOpt.outW = snap.profile.embedderInputW > 0 ? snap.profile.embedderInputW : 112;
    alignOpt.outH = snap.profile.embedderInputH > 0 ? snap.profile.embedderInputH : 112;

    for (auto& df : faces) {
        FaceDetection det;
        det.bbox = cv::Rect2f(static_cast<float>(df.bbox.x), static_cast<float>(df.bbox.y),
                              static_cast<float>(df.bbox.width), static_cast<float>(df.bbox.height));
        // 与 captureEnrollmentFaces 同构：仅在 hasKeypoints 时填充关键点，保证两侧
        // alignFaceForArcFace112 选择同一分支（关键点仿射 vs bbox fallback）。
        if (df.hasKeypoints) {
            std::array<cv::Point2f, 5> kp;
            for (std::size_t i = 0; i < 5; ++i) {
                kp[i] = cv::Point2f(df.keypoints[i * 2], df.keypoints[i * 2 + 1]);
            }
            det.keypoints5 = kp;
        }
        const FaceAlignResult ar = alignFaceForArcFace112(frame, det, alignOpt);
        if (ar.alignedBgr.empty()) {
            // 对齐失败早退（逐脸路径**第 6 处**丢弃，此前只有裸 continue ⇒ 静默）。
            // 与 captureEnrollmentFaces 同口径：无向量可清，只补记一条限频日志，共用同一
            // noteEmbedError 时间窗（**不新增上界**）。err 必非空——FaceAlign.cpp 中凡返回空
            // alignedBgr 的出口（输入图像为空 / 输出尺寸非法 / bbox 越界或为空 / 裁剪-resize
            // 失败）都设了 `r.err`。
            noteEmbedError("assignRealtimeIdentities/align", ar.err);
            continue;
        }

        std::vector<float> emb;
        std::string eerr;
        if (!snap.embedder->embed(ar.alignedBgr, emb, eerr)) {
            // 与 captureEnrollmentFaces 同口径：embed 失败时 out 的契约是「未定义」——生产包装
            // EmbedderRealtimeAdapter 只在成功时移动赋值、失败不写，但**注入式**实现（含宿主
            // 自定义）可能已写入部分明文；丢弃前清零（模板红线）。std::fill 对空 vector 是
            // no-op，故生产失败路径（该包装不写 out ⇒ emb 为空）零开销。
            std::fill(emb.begin(), emb.end(), 0.0f);
            noteEmbedError("assignRealtimeIdentities/embed", eerr);
            continue;
        }

        std::string serr;
        const std::vector<FaceSearchHit> hits =
            gallery->searchTopK(emb, 1, FaceSearchOptions{}, serr);
        // 检索失败（serr 非空）时 hits 为空、身份保持 Unknown——合法结果，但不可静默：
        // 记一条限频日志（与 embed 失败共用同一限频器，仅进日志、不外传）。
        noteEmbedError("assignRealtimeIdentities/search", serr);
        if (!hits.empty() && hits[0].score >= snap.profile.identitySimilarity) {
            df.identityId = hits[0].id;
            df.confidence = hits[0].score;
            df.isAuthenticated = true;
        }
    }
}

bool Engine::preprocessFrame(const cv::Mat& frame) {
    if (frame.cols != rk_core::config::FRAME_WIDTH || frame.rows != rk_core::config::FRAME_HEIGHT) {
        cv::resize(frame, processedFrame_, cv::Size(rk_core::config::FRAME_WIDTH, rk_core::config::FRAME_HEIGHT),
                   0.0, 0.0, cv::INTER_AREA);
    } else {
        processedFrame_ = frame;
    }
    // 变换输出新 Mat：避免原地修改与发布缓冲（`pipeline::ResultPublisher::renderFrame_`，**不是**
    // Engine 的成员）共享的 frame 数据，
    // 消除渲染线程 copy 相位漂移导致的画面按拍频来回镜像（RK3288 高频翻转回归）。
    // 变换完全由配置决定（不读取检测/轨迹）；水平实际翻转 = input XOR target。
    // 旋转已在 toBgrFromExternalFrame 处理，故此处 rotation_degrees 恒为 0，避免二次旋转。
    FrameTransformConfig cfg;
    cfg.input_mirrored_x = inputMirroredX_.load();
    cfg.target_mirrored_x = flipXEnabled_.load();
    cfg.flip_y = flipYEnabled_.load();
    processedFrame_ = FrameTransform::applyImage(processedFrame_, cfg);
    return !processedFrame_.empty();
}

void Engine::trackFaces(const std::vector<pipeline::DetectedFace>& faces, bool detectionRan,
                        uint64_t frameGalleryGen) {
    // 任务5/12：detectionRan = 本次是否实际执行检测。
    // ⚠️ 生产路径**恒传 true**，本参数当前是恒真入参，理由有三：
    //   ① 实时路径只在 runRealtimeDetect 成功时调用本函数，而该函数仅在「真实检测已执行」
    //      的唯一成功出口把 detectionRan 置 true（含 boxes 为空的**实际空检测**，使旧框按
    //      连续缺失规则消失，区别于「跳过检测」）；
    //   ② 兼容路径显式传 true；
    //   ③ 模型缺失 / 管线不可用 → runRealtimeDetect 返回 false，**根本不调用本函数**，而是走
    //      processFrameRealtime 的 currentTracks_.clear()（清空**对外视图**；coordinator 内部
    //      轨迹不动，仍受 TTL 约束）。故**不存在**「模型缺失时 detectionRan=false」这条路径。
    // 由此：TrackCoordinator::update 中 `if (!detectionRan) return views();` 的早退分支在生产
    // 路径上不可达，仅测试可构造。**该早退与 TTL 清理的相对顺序已被用例双向钉住**：
    // tests/cpp/test_track_coordinator.cpp 的 TtlStillCullsTracksAsFallback 与 TtlBoundaryIsEnforced
    // 都在 detectionRan=false 且超 ttlMs 时断言轨迹被清理——把早退上移到 TTL 之前即令二者翻红，
    // 故顺序不变量不是「仅靠注释自觉」。改动调用点前请先确认这三条是否仍成立。
    // 持 tracksMu_ 覆盖 trackCoordinator_->update 与 currentTracks_ 的更新：update 读写
    // coordinator 内部轨迹，而 applyGalleryUpdate()（JNI 线程）会在同一把锁下换库并精准改身份，
    // 二者必须互斥，否则对 tracks_ 形成数据竞争（第三波审查 #2）。
    std::lock_guard<std::mutex> lock(tracksMu_);
    // #4 残留竞态闭合（读路径代次作废）：若图库在「读库(assignRealtimeIdentities)」与
    // 「提交轨迹(此处)」之间被替换，本帧的认证结论据**旧库**得出、已失效——须先降级为
    // Unknown 再交 coordinator，否则 update() 会把已删除/已移除者的身份重新写入轨迹
    //（stableId=="Unknown" 且该帧 isAuthenticated 时会重新赋身份），使撤销失效。
    // 代次一致（正常帧）时零拷贝、零开销；兼容路径传 kNoGalleryGuard，恒不触发。
    const std::vector<pipeline::DetectedFace>* effective = &faces;
    std::vector<pipeline::DetectedFace> staleFaces;
    if (frameGalleryGen != kNoGalleryGuard &&
        frameGalleryGen != galleryGeneration_.load(std::memory_order_acquire)) {
        staleFaces = faces;
        for (auto& f : staleFaces) {
            f.isAuthenticated = false;
            f.identityId = "Unknown";
        }
        effective = &staleFaces;
    }
    const std::vector<pipeline::TrackView> views =
        trackCoordinator_->update(*effective, nowMs(), detectionRan);
    currentTracks_ = views;
}

namespace {
// 把快照内的稳定身份清为 "Unknown"（保留 trackId/bbox），语义与 revokeTrackIdentitiesLocked 一致。
void clearStableIdentities(std::vector<pipeline::TrackView>& tracks) {
    for (auto& v : tracks) {
        if (v.stableId != "Unknown") {
            v.stableId = "Unknown";
            v.stableConfidence = 0.0f;
        }
    }
}
}  // namespace

void Engine::renderResults() {
    pipeline::FrameOutcome outcome;
    uint64_t snapshotIdentityVer = 0;
    {
        std::lock_guard<std::mutex> lock(tracksMu_);
        outcome.tracks = currentTracks_;
        // 记录快照时刻的身份版本：与拷贝同锁读取，保证「身份内容」与「版本」一致。
        snapshotIdentityVer = identityVersion_.load(std::memory_order_acquire);
    }
    // 测试接缝：在「快照拷贝后、发布前」窗口内可插入受控同步。生产构建**不定义**
    // RK_ENABLE_TEST_SEAMS → 无此调用、无虚函数、无 vptr（零开销）；仅 core_gtest_tests 定义该宏
    //（其编译 test_engine.cpp 的 BarrierEngine 覆写此钩子）。
#if defined(RK_ENABLE_TEST_SEAMS)
    onBeforeRenderPublish();
#endif

    // 发布前校验①（绘制前）：若版本已变，说明撤销发生在拷贝之后，本次快照的稳定身份已失效；
    // 先清为 Unknown 再绘制，避免已撤销身份被画进 renderFrame（发布路径 P1：拷贝在 tracksMu_
    // 内、绘制/发布在锁外）。
    if (snapshotIdentityVer != identityVersion_.load(std::memory_order_acquire)) {
        clearStableIdentities(outcome.tracks);
    }
    if (!outcome.tracks.empty()) {
        // 绘制前断开与输入帧的共享：无 flip 时 processedFrame_ 别名输入帧（浅拷贝），
        // 就地画框会写共享缓冲，与渲染线程 copyTo 并发撕裂（A1/A2 审计）；仅有人脸帧
        // 才 clone，无脸路径保持零拷贝
        processedFrame_ = processedFrame_.clone();
        // 在发布帧上绘制人脸检测框（认证绿框/未认证红框 + 标签）——
        // 三层重构（029e43f）删除内联绘制后运行时无任何检测框，此处恢复
        drawTracks(processedFrame_, outcome.tracks);
    }
    outcome.renderFrame = processedFrame_;
    outcome.stats = currentStats_;
    // 发布前校验② + 事件发射（第五轮审查 P1）：二者必须在**同一 identityEmitMu_ 临界区**内完成。
    // 此前只做「检后再发」，JNI 线程可插在检查与 publish 之间完成 applyGalleryUpdate 并**返回**，
    // 本帧仍按旧副本发出已撤销身份的 VERIFIED。现在：撤销要么等本临界区结束才返回，要么先递增
    // 版本使本处校验失败→先清身份再发布。两条路径都不会在撤销返回后发出旧 VERIFIED。
    {
        std::lock_guard<std::mutex> emitGate(identityEmitMu_);
        // FACE-PIPE-015：进入发射临界区即置位本线程标记（RAII）。此后经 publish 触发的结果回调若
        // 同步回入取 tracksMu_ 的公开 API，将被入口检测拒绝，而非 ABBA 死锁。
        // 声明顺序写在 emitGate 之后，只为与「先持门、后标记」的语义读起来自洽；因两者析构之间**没有
        // 任何代码**，标记在门之前构造其实等价——**不要**把这里的顺序当作某个真实窗口的屏障
        //（A1 审计指出先前的注释作了一条不承重的「构造顺序 ⇒ 反向析构」论证）。
        IdentityEmitGateScope emitScope;
#if defined(RK_ENABLE_TEST_SEAMS)
        // 测试接缝：钉在发射临界区内（见 onBeforePublishLocked 注释）。
        onBeforePublishLocked();
#endif
        if (snapshotIdentityVer != identityVersion_.load(std::memory_order_acquire)) {
            clearStableIdentities(outcome.tracks);
            // FACE-PIPE-017：校验② 只在绘制**之后**才清身份——若不重绘，上面 drawTracks
            //（本函数内 `drawTracks(processedFrame_, outcome.tracks)`）已画上的认证**绿框**会随
            // 发布帧发出，而对外视图已变 Unknown，形成同帧「绿框 + AUTH_FAIL Unknown」的自相矛盾。此处
            // 就地重绘，令**发布帧像素**与对外视图在本帧内一致。放门内，是因为校验② 是发布前最后一道
            // 判定：撤销必须在 publish 返回前就不残留旧 VERIFIED，重绘必须与清身份同处这一不可分的窗口。
            // 必须与绘制路径（本函数内 `if (!outcome.tracks.empty()) { processedFrame_ = processedFrame_.clone();
            // drawTracks(...); }`）同条件：仅人脸帧才 clone 出独立缓冲，重绘才安全；无脸帧
            // （tracks.empty()）的 renderFrame 仍与输入帧别名（浅拷贝），就地绘制会写共享缓冲、与渲染
            // 线程并发撕裂（见该处 clone 的理由注释）。故此处**不 clone**——只覆写已 clone 的那块缓冲，
            // 保持临界区轻量。
            // ⚠️ 本段**刻意以符号 / 代码片段而非行号作锚点**：本仓已有「注释因行号漂移而失真」的先例
            //    （见 native_face_bridge.cpp 中同类说明），且本文件刚因增行让旧锚点（1407/1400/1404 等）
            //    全部失准，本批一并订正。
            // ⚠️ 不追求像素级还原：drawTracks 对未认证身份用较细线宽（2，认证为 3）且标签更短，故原认证框
            //    的最外 1px 与更长标签的残余可能留下。本重绘消除的是「被读成已认证」的误导，不是把该区域
            //    恢复成绘制前的像素——那需要门内 clone，与上面的轻量约束冲突。
            if (!outcome.tracks.empty()) {
                drawTracks(outcome.renderFrame, outcome.tracks);
            }
        }
        publisher_->publish(std::move(outcome));
    }
}

void Engine::collectStats() {
    perfReporter_->submit(currentStats_);
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
    const bool oldX = flipXEnabled_.exchange(flipX);
    const bool oldY = flipYEnabled_.exchange(flipY);
    if (oldX != flipX || oldY != flipY) {
        flipGeneration_.fetch_add(1);  // 变换配置变化时 generation 递增
    }
}

void Engine::setInputMirroredX(bool mirrored) {
    const bool old = inputMirroredX_.exchange(mirrored);
    if (old != mirrored) {
        flipGeneration_.fetch_add(1);
    }
}

cv::Mat Engine::applyFlip(const cv::Mat& frame, bool flipX, bool flipY) {
    if (!flipX && !flipY) {
        return frame;
    }
    int code = (flipX && flipY) ? -1 : (flipY ? 0 : 1);
    cv::Mat flipped;
    cv::flip(frame, flipped, code);
    return flipped;
}

void Engine::drawTracks(cv::Mat& frame, const std::vector<pipeline::TrackView>& tracks) {
    for (const auto& t : tracks) {
        const bool authed = (t.stableId != "Unknown");
        const cv::Scalar color = authed ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
        const int thickness = authed ? 3 : 2;
        cv::rectangle(frame, t.bbox, color, thickness);

        std::ostringstream label;
        label << "T" << t.trackId << " " << t.stableId;
        if (authed) {
            label << " " << static_cast<int>(t.stableConfidence * 100) << "%";
        }
        const cv::Point origin(std::max(0, t.bbox.x), std::max(0, t.bbox.y - 8));
        cv::putText(frame, label.str(), origin, cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 2);
    }
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
