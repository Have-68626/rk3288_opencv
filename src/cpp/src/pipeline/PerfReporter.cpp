#include "pipeline/PerfReporter.h"
#include <cstdio>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <chrono>

namespace pipeline {

PerfReporter::PerfReporter(const std::string& csvDir) : csvDir_(csvDir) {
    csvPath_ = csvDir_ + "/engine_perf.csv";
    worker_ = std::thread(&PerfReporter::workerLoop, this);
}

PerfReporter::~PerfReporter() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void PerfReporter::submit(const PerfStats& s) {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(s);
    if (queue_.size() > 1024) queue_.pop_front();
}

void PerfReporter::workerLoop() {
    while (running_) {
        std::deque<PerfStats> batch;
        {
            std::lock_guard<std::mutex> lk(mu_);
            batch.swap(queue_);
        }
        if (batch.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // 聚合
        std::vector<double> totals;
        totals.reserve(batch.size());
        for (const auto& s : batch) {
            totals.push_back(s.decodeMs + s.preMs + s.inferMs + s.postMs + s.renderMs);
        }
        std::sort(totals.begin(), totals.end());
        if (totals.empty()) continue;

        double mean = std::accumulate(totals.begin(), totals.end(), 0.0) / totals.size();
        double p50 = totals[totals.size() / 2];
        double p95 = totals[static_cast<size_t>(totals.size() * 0.95)];
        double maxVal = totals.back();

        std::cout << "Perf Total: Mean=" << mean << "ms, P50=" << p50
                  << "ms, P95=" << p95 << "ms, Max=" << maxVal
                  << "ms | Peak RSS: " << (batch.back().rssBytes / 1024 / 1024) << "MB" << std::endl;

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
