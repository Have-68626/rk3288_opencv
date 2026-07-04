#pragma once
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <deque>
#include "pipeline/ResultPublisher.h"  // PerfStats

namespace pipeline {

class PerfReporter {
public:
    explicit PerfReporter(const std::string& csvDir = "tests/metrics");
    ~PerfReporter();

    // 热路径安全——无锁入队
    void submit(const PerfStats& s);

private:
    void workerLoop();

    std::deque<PerfStats> queue_;
    std::mutex mu_;
    std::thread worker_;
    std::atomic<bool> running_{true};
    std::string csvPath_;
    std::string csvDir_;
};

}  // namespace pipeline
