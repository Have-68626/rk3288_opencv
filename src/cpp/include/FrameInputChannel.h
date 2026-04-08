/**
 * @file FrameInputChannel.h
 * @brief 外部帧输入通道：生产者(JNI/采集线程) -> 消费者(Engine 推理线程)。
 *
 * 线程与背压约定：
 * - 生产者通过 push() 喂入帧；消费者通过 waitPop()/tryPop() 取帧。
 * - 支持两种背压策略：
 *   1) LatestOnly：仅保留最新帧（老帧直接被覆盖），最适合“预览/推理只看最新”的场景。
 *   2) BoundedQueue：有限队列（超过上限丢弃最旧帧），用于需要短暂缓冲的场景。
 *
 * JNI 所有权与生命周期：
 * - 本通道假设入帧对象及其数据由 Native 持有（例如 JNI 层做拷贝后再 push），
 *   避免 Java Buffer 被复用/释放导致的悬挂指针。
 */
#pragma once

#include "ExternalFrame.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

enum class FrameBackpressureMode : int32_t {
    LatestOnly = 0,
    BoundedQueue = 1,
};

struct FrameChannelStats {
    uint64_t pushed = 0;
    uint64_t popped = 0;
    uint64_t dropped = 0;
};

class FrameInputChannel {
public:
    FrameInputChannel();

    void configure(FrameBackpressureMode mode, std::size_t capacity);
    FrameBackpressureMode mode() const;
    std::size_t capacity() const;

    void clear();
    FrameChannelStats stats() const;

    void push(ExternalFrame frame);
    bool tryPop(ExternalFrame& out);
    bool waitPop(ExternalFrame& out, int timeoutMs);

private:
    void dropOldestLocked_();

    mutable std::mutex mu_;
    std::condition_variable cv_;

    FrameBackpressureMode mode_ = FrameBackpressureMode::LatestOnly;
    std::size_t capacity_ = 1;

    bool hasLatest_ = false;
    ExternalFrame latest_{};
    std::deque<ExternalFrame> q_{};

    std::atomic<uint64_t> pushed_{0};
    std::atomic<uint64_t> popped_{0};
    std::atomic<uint64_t> dropped_{0};
};

