#pragma once
#include <cstdint>
#include <atomic>
#include <functional>

#ifndef RK_WIN_HAS_OPENCV
#if __has_include(<opencv2/core.hpp>)
#define RK_WIN_HAS_OPENCV 1
#else
#define RK_WIN_HAS_OPENCV 0
#endif
#endif

#if RK_WIN_HAS_OPENCV
#include <opencv2/core.hpp>
#else
namespace cv {
struct Mat {
    int rows = 0;
    int cols = 0;
    void* data = nullptr;
};
}
#endif

namespace rk_win {

class FramePipeline;

enum class StreamType { Sse, Mjpeg };

class StreamSessionRunner {
public:
    // provider: 获取当前帧（阻塞或非阻塞均可）
    using FrameProvider = std::function<bool(cv::Mat&)>;

    StreamSessionRunner(std::atomic<bool>& running, FrameProvider provider);

    // 设置 FramePipeline 指针，供 SSE 会话使用
    void setPipe(FramePipeline* pipe) { pipe_ = pipe; }

    // 执行流式会话（阻塞直到客户端断开或服务停止）
    void run(std::uintptr_t sock, StreamType type);

private:
    void runSse(std::uintptr_t sock);
    void runMjpeg(std::uintptr_t sock);
    bool writeRaw(std::uintptr_t sock, const void* data, std::size_t n);

    std::atomic<bool>& running_;
    FrameProvider provider_;
    FramePipeline* pipe_ = nullptr;
};

}  // namespace rk_win
