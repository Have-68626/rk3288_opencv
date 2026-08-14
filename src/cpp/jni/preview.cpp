/**
 * @file jni/preview.cpp
 * @brief JNI 预览与渲染（preview domain）
 *
 * 管理 Android Surface/Bitmap 的预览帧渲染，包括 ANativeWindow
 * 和 Bitmap 的锁定/解锁与色彩空间转换。
 */
#include <jni.h>
#include <android/native_window.h>
#include <android/bitmap.h>
#include <android/native_window_jni.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>
#include <mutex>
#include <cstring>

#include "Engine.h"
#include "NativeLog.h"
#include "JniMethodRegistry.h"
#include "jni_shared.h"
#include "PreviewGeomCache.h"
#include "rk_checked_arith.h"

#define TAG "RK3288_JNI"

namespace {

// ========== RAII 资源封装 ==========

// ScopedWindowLock — 自动 ANativeWindow_lock/unlockAndPost
class ScopedWindowLock {
    ANativeWindow* win_;
    ANativeWindow_Buffer buf_;
public:
    explicit ScopedWindowLock(ANativeWindow* win) : win_(win) {
        if (ANativeWindow_lock(win_, &buf_, nullptr) != 0) {
            win_ = nullptr;
        }
    }
    ANativeWindow_Buffer* buffer() { return win_ ? &buf_ : nullptr; }
    bool isLocked() const { return win_ != nullptr; }
    ~ScopedWindowLock() {
        if (win_) ANativeWindow_unlockAndPost(win_);
    }
    ScopedWindowLock(const ScopedWindowLock&) = delete;
    ScopedWindowLock& operator=(const ScopedWindowLock&) = delete;
    ScopedWindowLock(ScopedWindowLock&&) = delete;
    ScopedWindowLock& operator=(ScopedWindowLock&&) = delete;
};

// ScopedBitmapLock — 自动 AndroidBitmap_lockPixels/unlockPixels
class ScopedBitmapLock {
    JNIEnv* env_;
    jobject bitmap_;
    void* pixels_;
public:
    explicit ScopedBitmapLock(JNIEnv* env, jobject bitmap)
        : env_(env), bitmap_(bitmap), pixels_(nullptr) {
        if (AndroidBitmap_lockPixels(env_, bitmap_, &pixels_) != 0) {
            pixels_ = nullptr;
        }
    }
    void* data() const { return pixels_; }
    bool isLocked() const { return pixels_ != nullptr; }
    ~ScopedBitmapLock() {
        if (pixels_) AndroidBitmap_unlockPixels(env_, bitmap_);
    }
    ScopedBitmapLock(const ScopedBitmapLock&) = delete;
    ScopedBitmapLock& operator=(const ScopedBitmapLock&) = delete;
    ScopedBitmapLock(ScopedBitmapLock&&) = delete;
    ScopedBitmapLock& operator=(ScopedBitmapLock&&) = delete;
};

// ========== nativeSetPreviewSurface ==========
void preview_nativeSetPreviewSurface(JNIEnv* env, jobject /* this */, jobject surface) {
    std::lock_guard<std::mutex> lock(g_previewMutex);
    if (g_previewWindow) {
        ANativeWindow_release(g_previewWindow);
        g_previewWindow = nullptr;
    }
    g_previewGeneration.fetch_add(1, std::memory_order_relaxed);
    if (!surface) return;
    g_previewWindow = ANativeWindow_fromSurface(env, surface);
}

// ========== nativeRenderFrameToSurface ==========
jboolean preview_nativeRenderFrameToSurface(JNIEnv* /* env */, jobject /* this */) {
    if (!g_engine) return JNI_FALSE;

    ANativeWindow* win = nullptr;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        win = g_previewWindow;
        if (win) {
            ANativeWindow_acquire(win);
        }
        gen = g_previewGeneration.load(std::memory_order_relaxed);
    }
    if (!win) return JNI_FALSE;

    struct WindowReleaser {
        ANativeWindow* w;
        ~WindowReleaser() { if (w) ANativeWindow_release(w); }
    } releaser{win};

    cv::Mat frame;
    uint64_t seq = 0;
    if (!g_engine->getRenderFrame(frame, seq)) {
        return JNI_FALSE;
    }
    if (frame.empty()) return JNI_FALSE;

    // 序号去重
    {
        static std::mutex seqMu;
        static uint64_t lastSeqGlob = static_cast<uint64_t>(-1);
        static int lastGenGlob = -1;
        static int lastWGlob = 0;
        static int lastHGlob = 0;
        static int lastFormatGlob = 0;
        std::lock_guard<std::mutex> lock(seqMu);
        if (gen != lastGenGlob) {
            lastGenGlob = gen;
            lastSeqGlob = static_cast<uint64_t>(-1);
            lastWGlob = 0;
            lastHGlob = 0;
            lastFormatGlob = 0;
        }
        if (seq == lastSeqGlob) {
            return JNI_TRUE;
        }
        lastSeqGlob = seq;
    }

    const int w = frame.cols;
    const int h = frame.rows;
    if (w <= 0 || h <= 0) return JNI_FALSE;

    // 几何缓存：记录已下发到 ANativeWindow 的缓冲几何（代次+尺寸+格式），
    // 用于跳过重复的 setBuffersGeometry。
    // Q-CQ-B07-003 语义：Surface 重建（g_previewGeneration 代次变化）后旧缓存
    // 必须失效并重设，否则新 Surface 沿用旧 geometry，尺寸/格式守卫（stride、
    // format 校验）持续失败，预览卡黑。判定以代次为第一失效维度——即使帧尺寸
    // 与旧代次相同，重建后也必须重新下发，保证新 Surface 尺寸生效。
    static rk_jni::PreviewGeomCache geomCache;
    if (geomCache.shouldSetGeometry(gen, w, h, WINDOW_FORMAT_RGBA_8888)) {
        if (ANativeWindow_setBuffersGeometry(win, w, h, WINDOW_FORMAT_RGBA_8888) != 0) {
            return JNI_FALSE;
        }
        geomCache.update(gen, w, h, WINDOW_FORMAT_RGBA_8888);
    }

    ScopedWindowLock lockedWin(win);
    if (!lockedWin.isLocked()) {
        return JNI_FALSE;
    }
    ANativeWindow_Buffer* buffer = lockedWin.buffer();

    const int dstStridePixels = buffer->stride;
    // S-SEC-B09-003：32 位 checked 乘法——stride*4 溢出前拒绝，防止负偏移/
    // 回绕越界写；主路径与清空回退路径共用该乘积（cv::Mat step 亦需安全）
    int dstStrideBytes = 0;
    if (!rk_jni::mulS32Checked(dstStridePixels, 4, dstStrideBytes)) {
        return JNI_FALSE;
    }
    if (dstStridePixels < w || buffer->bits == nullptr || buffer->format != WINDOW_FORMAT_RGBA_8888) {
        if (buffer->bits != nullptr && dstStrideBytes > 0) {
            for (int row = 0; row < h; row++) {
                int rowOffset = 0;
                if (!rk_jni::mulS32Checked(row, dstStrideBytes, rowOffset)) {
                    // 偏移溢出：放弃清空，不写缓冲（受控失败）
                    return JNI_FALSE;
                }
                // 仅清空行内有效 stride 范围（stride<w 异常时亦不越界写缓冲）
                std::memset(reinterpret_cast<uint8_t*>(buffer->bits) + rowOffset, 0,
                            static_cast<std::size_t>(dstStrideBytes));
            }
        }
        return JNI_FALSE;
    }

    // S-SEC-B09-003：主路径 32 位 checked 尺寸乘法——stride*4 与 (stride*4)*h
    // 均须在 int32 内，否则 cv::Mat step/总字节在 32 位 size_t 回绕导致越界写
    int dstBufBytes = 0;
    if (!rk_jni::mulS32Checked(dstStrideBytes, h, dstBufBytes)) {
        return JNI_FALSE;
    }

    cv::Mat dst(h, dstStridePixels, CV_8UC4, buffer->bits);
    cv::Mat dstRoi = dst(cv::Rect(0, 0, w, h));
    cv::cvtColor(frame, dstRoi, cv::COLOR_BGR2RGBA);

    // ScopedWindowLock 析构自动 ANativeWindow_unlockAndPost
    return JNI_TRUE;
}

// ========== nativeGetFrame ==========
jboolean preview_nativeGetFrame(JNIEnv* env, jobject /* this */, jobject bitmap) {
    if (!g_engine) return false;

    cv::Mat frame;
    uint64_t seq = 0;
    if (!g_engine->getRenderFrame(frame, seq)) {
        return false;
    }
    {
        static std::mutex seqMu;
        static uint64_t lastSeqGlob = static_cast<uint64_t>(-1);
        std::lock_guard<std::mutex> lock(seqMu);
        if (seq == lastSeqGlob) {
            return false;
        }
        lastSeqGlob = seq;
    }

    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) return false;
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) return false;

    ScopedBitmapLock lockedBitmap(env, bitmap);
    if (!lockedBitmap.isLocked()) return false;
    void* pixels = lockedBitmap.data();

    cv::Mat tmp;
    if (frame.cols != info.width || frame.rows != info.height) {
        cv::resize(frame, tmp, cv::Size(info.width, info.height));
    } else {
        tmp = frame;
    }

    cv::Mat rgbaFrame(info.height, info.width, CV_8UC4, pixels);
    cv::cvtColor(tmp, rgbaFrame, cv::COLOR_BGR2RGBA);

    // ScopedBitmapLock 析构自动 unlockPixels
    return true;
}

} // anonymous namespace

void registerPreviewMethods(JniMethodRegistry& registry) {
    registry.add({
        {"nativeSetPreviewSurface", "(Landroid/view/Surface;)V",
         reinterpret_cast<void*>(preview_nativeSetPreviewSurface)},
        {"nativeRenderFrameToSurface", "()Z",
         reinterpret_cast<void*>(preview_nativeRenderFrameToSurface)},
        {"nativeGetFrame", "(Landroid/graphics/Bitmap;)Z",
         reinterpret_cast<void*>(preview_nativeGetFrame)},
    });
}
