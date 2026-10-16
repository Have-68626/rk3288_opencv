#pragma once
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <deque>
#include <condition_variable>
#include <cstdint>
#include "pipeline/ResultPublisher.h"  // PerfStats

namespace pipeline {

class PerfReporter {
public:
    explicit PerfReporter(const std::string& csvDir = "tests/metrics");
    ~PerfReporter();

    // 短锁入队；保留最新 1024 个样本，丢弃数由后台日志披露，聚合与 I/O 均在锁外。
    void submit(const PerfStats& s);

private:
    void workerLoop();

    std::deque<PerfStats> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::uint64_t droppedSamples_ = 0;
    std::thread worker_;
    std::atomic<bool> running_{true};
    std::string csvPath_;
    std::string csvDir_;
};

}  // namespace pipeline
