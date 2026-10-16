#include "pipeline/PerfReporter.h"
#include <cstdio>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <chrono>
#include <cmath>

namespace pipeline {

PerfReporter::PerfReporter(const std::string& csvDir) : csvDir_(csvDir) {
    csvPath_ = csvDir_ + "/engine_perf.csv";
    worker_ = std::thread(&PerfReporter::workerLoop, this);
}

PerfReporter::~PerfReporter() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        running_ = false;
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void PerfReporter::submit(const PerfStats& s) {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(s);
    if (queue_.size() > 1024) {
        queue_.pop_front();
        ++droppedSamples_;
    }
}

void PerfReporter::workerLoop() {
    long long sampledPeakRss = 0;
    while (true) {
        std::deque<PerfStats> batch;
        std::uint64_t dropped = 0;
        {
            std::unique_lock<std::mutex> lk(mu_);
            // 保持每秒批量聚合；停止信号立即唤醒，排空尾批次后才退出。
            cv_.wait_for(lk, std::chrono::seconds(1), [this] { return !running_.load(); });
            if (!running_ && queue_.empty()) break;
            batch.swap(queue_);
            dropped = droppedSamples_;
        }
        if (batch.empty()) continue;

        // 聚合
        std::vector<double> totals;
        totals.reserve(batch.size());
        for (const auto& s : batch) {
            totals.push_back(s.decodeMs + s.preMs + s.inferMs + s.postMs + s.renderMs);
            sampledPeakRss = std::max(sampledPeakRss, s.rssBytes);
        }
        std::sort(totals.begin(), totals.end());
        if (totals.empty()) continue;

        double mean = std::accumulate(totals.begin(), totals.end(), 0.0) / totals.size();
        // 与 RealtimeFacePipeline 一致：最近秩 ceil(p*n)-1，偶数样本的 p50 取下中位值。
        double p50 = totals[static_cast<size_t>(std::ceil(totals.size() * 0.50)) - 1];
        double p95 = totals[static_cast<size_t>(std::ceil(totals.size() * 0.95)) - 1];
        double maxVal = totals.back();

        std::cout << "Perf Total: Mean=" << mean << "ms, P50=" << p50
                  << "ms, P95=" << p95 << "ms, Max=" << maxVal
                  << "ms | Sampled Peak RSS: " << (sampledPeakRss / 1024 / 1024)
                  << "MB | Samples: " << batch.size() << " | Dropped: " << dropped << std::endl;

        // 写 CSV
        FILE* f = fopen(csvPath_.c_str(), "a");
        if (f) {
            for (const auto& s : batch) {
                fprintf(f, "%g,%g,%g,%g,%g,%lld\n",
                    s.decodeMs, s.preMs, s.inferMs, s.postMs, s.renderMs,
                    static_cast<long long>(s.rssBytes));
            }
            fclose(f);
        }
    }
}

}  // namespace pipeline
