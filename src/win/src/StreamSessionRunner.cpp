#include "rk_win/StreamSessionRunner.h"
#include "rk_win/FacesJson.h"
#include "rk_win/FramePipeline.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#define SOCKET int
#endif

#if RK_WIN_HAS_OPENCV
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace rk_win {
namespace {

constexpr std::size_t kSendChunkSize = 1u << 20;   // 1 MiB
constexpr int kSsePollMs = 1000;
constexpr int kSseIdleMs = 100;
constexpr auto kSseKeepaliveSec = std::chrono::seconds(10);
constexpr int kMjpegIdleMs = 100;

}  // namespace

StreamSessionRunner::StreamSessionRunner(std::atomic<bool>& running, FrameProvider provider,
                                         StreamLimits limits)
    : running_(running), provider_(std::move(provider)), limits_(limits) {}

void StreamSessionRunner::run(std::uintptr_t sock, StreamType type) {
    switch (type) {
    case StreamType::Sse:   runSse(sock);   break;
    case StreamType::Mjpeg: runMjpeg(sock); break;
    }
}

bool StreamSessionRunner::sessionExpired(const std::chrono::steady_clock::time_point& started) const {
    const auto dur = limits_.maxSessionDuration;
    if (dur.count() <= 0) return false;
    return std::chrono::steady_clock::now() - started >= dur;
}

bool StreamSessionRunner::writeRaw(std::uintptr_t sock, const void* data, std::size_t n) {
    const std::size_t maxBytes = limits_.maxBytesPerSession;
    if (maxBytes > 0) {
        // S-SEC-B09-005：单会话字节配额记账。配额耗尽 → 拒绝本次写入并返回 false，
        // 调用方以"会话终止"处理（客户端随即收到连接关闭/EOF）。
        // 防失控或被劫持的会话迫使服务端无限产出数据（回退硬顶，见 StreamLimits 注释）。
        if (bytesWritten_ >= maxBytes || n > maxBytes - bytesWritten_) return false;
        bytesWritten_ += n;
    }
    const auto s = static_cast<SOCKET>(sock);
    const char* p = static_cast<const char*>(data);
    std::size_t left = n;
    while (left > 0) {
        const int w = ::send(s, p, static_cast<int>(std::min<std::size_t>(left, kSendChunkSize)), 0);
        if (w <= 0) return false;
        p += w;
        left -= static_cast<std::size_t>(w);
    }
    return true;
}

void StreamSessionRunner::runSse(std::uintptr_t sock) {
    // 防御：pipe_ 可能尚未注入（start() 调用顺序）
    if (!pipe_) {
        const char* shutdown = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        writeRaw(sock, shutdown, std::strlen(shutdown));
        return;
    }
    // HTTP SSE 头部
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Content-Security-Policy: default-src 'none'\r\n"
        "Connection: keep-alive\r\n\r\n";
    if (!writeRaw(sock, header, std::strlen(header))) return;

    std::uint64_t lastSeq = 0;
    auto lastKeep = std::chrono::steady_clock::now();
    // S-SEC-B09-005：会话超时锚点（与 MJPEG 共用 sessionExpired 时长判定）。
    const auto sessionStart = std::chrono::steady_clock::now();

    while (running_) {
        // S-SEC-B09-005：SSE/流超时终止 —— 会话存活超限即断开（独立于全局
        // ConnectionQuota 的流级时长限制，防无限持有线程/连接）。注意：当前迭代
        // 仍会先完成一次 waitFacesSeqChanged（≤1s 轮询）后再判超时，精度即轮询粒度。
        if (sessionExpired(sessionStart)) break;
        std::uint64_t seq = 0;
        const bool changed = pipe_->waitFacesSeqChanged(lastSeq, kSsePollMs, seq);
        if (!running_) break;
        if (changed) {
            lastSeq = seq;
            FacesSnapshot snap;
            if (pipe_->snapshotFaces(snap)) {
                const std::string body = buildFacesJson(snap);
                const std::string evt = "data: " + body + "\n\n";
                if (!writeRaw(sock, evt.data(), evt.size())) break;
            }
        } else {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastKeep > kSseKeepaliveSec) {
                const std::string ka = ": keepalive\n\n";
                lastKeep = now;
                if (!writeRaw(sock, ka.data(), ka.size())) break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kSseIdleMs));
    }
}

void StreamSessionRunner::runMjpeg(std::uintptr_t sock) {
#if RK_WIN_HAS_OPENCV
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=rk_boundary\r\n"
        "Cache-Control: no-cache\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Content-Security-Policy: default-src 'none'\r\n"
        "Connection: close\r\n\r\n";
    if (!writeRaw(sock, header, std::strlen(header))) return;

    // S-SEC-B09-005：MJPEG FPS 节流 —— 距上一帧未达帧间最小间隔前不再取帧/编码。
    // 首帧立即发送（nextFrameAt 初始化为 now），其后每帧间隔 ≥ 1000/maxFps ms，
    // 把每流 JPEG 编码 CPU 封顶（预算依据见 StreamLimits.mjpegMaxFps 注释）。
    const auto minInterval = std::chrono::milliseconds(
        limits_.mjpegMaxFps > 0 ? 1000 / limits_.mjpegMaxFps : 0);
    auto nextFrameAt = std::chrono::steady_clock::now();
    // S-SEC-B09-005：会话超时锚点（与 SSE 共用 sessionExpired 时长判定）。
    const auto sessionStart = std::chrono::steady_clock::now();

    while (running_) {
        // S-SEC-B09-005：流超时终止（独立于全局 ConnectionQuota 的流级时长限制）。
        if (sessionExpired(sessionStart)) break;
        if (minInterval.count() > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextFrameAt) {
                std::this_thread::sleep_for(nextFrameAt - now);
                continue;
            }
        }
        cv::Mat frame;
        if (!provider_(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kMjpegIdleMs));
            continue;
        }

        std::vector<uchar> jpeg;
        cv::imencode(".jpg", frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, 80});

        std::string part;
        part.reserve(128);
        part += "--rk_boundary\r\nContent-Type: image/jpeg\r\nContent-Length: ";
        part += std::to_string(jpeg.size());
        part += "\r\n\r\n";
        if (!writeRaw(sock, part.data(), part.size())) break;
        if (!writeRaw(sock, reinterpret_cast<const char*>(jpeg.data()), jpeg.size())) break;
        if (!writeRaw(sock, "\r\n", 2)) break;
        nextFrameAt = std::chrono::steady_clock::now() + minInterval;
    }
#else
    (void)sock;
#endif
}

}  // namespace rk_win
