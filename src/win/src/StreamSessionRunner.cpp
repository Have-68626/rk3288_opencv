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

StreamSessionRunner::StreamSessionRunner(std::atomic<bool>& running, FrameProvider provider)
    : running_(running), provider_(std::move(provider)) {}

void StreamSessionRunner::run(std::uintptr_t sock, StreamType type) {
    switch (type) {
    case StreamType::Sse:   runSse(sock);   break;
    case StreamType::Mjpeg: runMjpeg(sock); break;
    }
}

bool StreamSessionRunner::writeRaw(std::uintptr_t sock, const void* data, std::size_t n) {
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

    while (running_) {
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

    while (running_) {
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
    }
#else
    (void)sock;
#endif
}

}  // namespace rk_win
