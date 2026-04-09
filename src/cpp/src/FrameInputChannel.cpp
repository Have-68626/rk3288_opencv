/**
 * @file FrameInputChannel.cpp
 * @brief 外部帧输入通道实现。
 */
#include "FrameInputChannel.h"

#include <chrono>

FrameInputChannel::FrameInputChannel() = default;

void FrameInputChannel::configure(FrameBackpressureMode mode, std::size_t capacity) {
    std::lock_guard<std::mutex> lock(mu_);
    mode_ = mode;
    capacity_ = (capacity == 0) ? 1 : capacity;
    if (mode_ == FrameBackpressureMode::LatestOnly) {
        capacity_ = 1;
        q_.clear();
    }
    if (mode_ == FrameBackpressureMode::BoundedQueue) {
        hasLatest_ = false;
        if (capacity_ < 1) capacity_ = 1;
        while (q_.size() > capacity_) {
            q_.pop_front();
        }
    }
}

FrameBackpressureMode FrameInputChannel::mode() const {
    std::lock_guard<std::mutex> lock(mu_);
    return mode_;
}

std::size_t FrameInputChannel::capacity() const {
    std::lock_guard<std::mutex> lock(mu_);
    return capacity_;
}

void FrameInputChannel::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    hasLatest_ = false;
    latest_ = ExternalFrame{};
    q_.clear();
}

FrameChannelStats FrameInputChannel::stats() const {
    FrameChannelStats s{};
    s.pushed = pushed_.load();
    s.popped = popped_.load();
    s.dropped = dropped_.load();
    return s;
}

void FrameInputChannel::dropOldestLocked_() {
    if (mode_ == FrameBackpressureMode::BoundedQueue) {
        if (!q_.empty()) {
            q_.pop_front();
            dropped_.fetch_add(1);
        }
        return;
    }
    dropped_.fetch_add(1);
}

void FrameInputChannel::push(ExternalFrame frame) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (mode_ == FrameBackpressureMode::LatestOnly) {
            if (hasLatest_) {
                dropped_.fetch_add(1);
            }
            latest_ = std::move(frame);
            hasLatest_ = true;
        } else {
            if (q_.size() >= capacity_) {
                dropOldestLocked_();
            }
            q_.push_back(std::move(frame));
        }
        pushed_.fetch_add(1);
    }
    cv_.notify_one();
}

bool FrameInputChannel::tryPop(ExternalFrame& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (mode_ == FrameBackpressureMode::LatestOnly) {
        if (!hasLatest_) return false;
        out = std::move(latest_);
        hasLatest_ = false;
        popped_.fetch_add(1);
        return true;
    }

    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    popped_.fetch_add(1);
    return true;
}

bool FrameInputChannel::waitPop(ExternalFrame& out, int timeoutMs) {
    std::unique_lock<std::mutex> lock(mu_);
    auto pred = [&]() -> bool {
        if (mode_ == FrameBackpressureMode::LatestOnly) return hasLatest_;
        return !q_.empty();
    };

    if (timeoutMs <= 0) {
        cv_.wait(lock, pred);
    } else {
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), pred)) {
            return false;
        }
    }

    if (mode_ == FrameBackpressureMode::LatestOnly) {
        out = std::move(latest_);
        hasLatest_ = false;
    } else {
        out = std::move(q_.front());
        q_.pop_front();
    }
    popped_.fetch_add(1);
    return true;
}

