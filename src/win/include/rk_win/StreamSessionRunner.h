#pragma once
#include <cstdint>
#include <atomic>
#include <chrono>
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

// S-SEC-B09-005：流会话配额（每会话资源上限，独立于全局 ConnectionQuota=64）。
// 全局 ConnectionQuota 只限制"并发连接数"，本结构限制"单个流会话能消耗的
// CPU / 字节 / 时长"——三个预算共同保证 64 连接攻击下总工作有界：
//  - mjpegMaxFps：MJPEG 每流帧率上限（节流 = 帧间最小间隔 1000/fps ms）。
//    依据 docs/RK3288_CONSTRAINTS.md §1.3/§1.6：相机输入 720p@15-30fps，预览
//    通道人眼 ≥15fps 已平滑；20fps 把每流 JPEG 编码 CPU 封顶（720p q80 ≈ 20ms/帧
//    → 20fps ≈ 单核 40%），恶意客户端无法迫使服务端按源帧率（30+ fps）编码。
//    0 = 不限（仅测试注入）。
//  - maxBytesPerSession：单会话累计写字节上限（回退硬顶）。依据：20fps × 720p
//    （q80 约 100-250KB/帧）≈ 2-5MB/s，8GiB 对应连续预览 30-60 分钟，远超正常
//    预览会话；仅拦截失控/被劫持会话无限产出。0 = 不限（仅测试注入）。
//  - maxSessionDuration：会话最大存活时长（超时终止）。独立于 ConnectionQuota 的
//    流级时长限制：SSE（EventSource）断线自动重连，30 分钟刷新无缝；MJPEG <img>
//    无自动重连，正常连续预览短于该值（预览页已有"刷新预览"兜底）。
//    0 = 不限（仅测试注入）。
struct StreamLimits {
    int mjpegMaxFps = 20;
    std::size_t maxBytesPerSession = static_cast<std::size_t>(8) << 30;  // 8 GiB
    std::chrono::milliseconds maxSessionDuration{30 * 60 * 1000};        // 30 min
};

class StreamSessionRunner {
public:
    // provider: 获取当前帧（阻塞或非阻塞均可）
    using FrameProvider = std::function<bool(cv::Mat&)>;

    StreamSessionRunner(std::atomic<bool>& running, FrameProvider provider,
                        StreamLimits limits = {});

    // 设置 FramePipeline 指针，供 SSE 会话使用
    void setPipe(FramePipeline* pipe) { pipe_ = pipe; }

    // 执行流式会话（阻塞直到客户端断开、配额耗尽/超时或服务停止）
    void run(std::uintptr_t sock, StreamType type);

private:
    void runSse(std::uintptr_t sock);
    void runMjpeg(std::uintptr_t sock);
    bool writeRaw(std::uintptr_t sock, const void* data, std::size_t n);
    // 会话是否已达 maxSessionDuration（未设时长 → 恒 false）
    bool sessionExpired(const std::chrono::steady_clock::time_point& started) const;

    std::atomic<bool>& running_;
    FrameProvider provider_;
    FramePipeline* pipe_ = nullptr;
    StreamLimits limits_{};
    // 单会话累计写字节（配额记账，仅会话线程访问）
    std::size_t bytesWritten_ = 0;
};

}  // namespace rk_win
