#include "pipeline/PerfReporter.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>

namespace {

class ReportOutput : public std::streambuf {
public:
    bool waitForWorker() {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::seconds(4), [this] { return entered_; });
    }

    void releaseWorker() {
        std::lock_guard<std::mutex> lock(mu_);
        blocked_ = false;
        cv_.notify_all();
    }

    bool waitForReports(int count) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::seconds(4), [this, count] {
            return reports_ >= count;
        });
    }

    std::string text() {
        std::lock_guard<std::mutex> lock(mu_);
        return text_;
    }

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        std::unique_lock<std::mutex> lock(mu_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return !blocked_; });
        text_.append(data, static_cast<std::size_t>(size));
        return size;
    }

    int overflow(int value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char ch = traits_type::to_char_type(value);
        return xsputn(&ch, 1) == 1 ? value : traits_type::eof();
    }

    int sync() override {
        std::lock_guard<std::mutex> lock(mu_);
        ++reports_;
        cv_.notify_all();
        return 0;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool blocked_ = true;
    int reports_ = 0;
    std::string text_;
};

struct ReportResult {
    bool workerEntered = false;
    bool batchReported = false;
    double shutdownMs = 0;
    std::string output;
    std::vector<std::vector<double>> rows;
};

enum class StopMode { AfterBatch, DuringOutput, WhileIdle };

ReportResult report(const pipeline::PerfStats& first,
                    const std::vector<pipeline::PerfStats>& pending,
                    StopMode stopMode = StopMode::AfterBatch) {
    static std::atomic<unsigned> nextId{0};
    const auto dir = std::filesystem::temp_directory_path() /
        ("rk_perf_reporter_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(nextId.fetch_add(1)));
    std::filesystem::create_directory(dir);
    ReportOutput captured;
    auto* previous = std::cout.rdbuf(&captured);
    ReportResult result;
    {
        auto reporter = std::make_unique<pipeline::PerfReporter>(dir.string());
        reporter->submit(first);
        result.workerEntered = captured.waitForWorker();
        if (stopMode == StopMode::WhileIdle) {
            captured.releaseWorker();
            result.batchReported = captured.waitForReports(1);
            // 首批已输出，再让 worker 完成 CSV 并进入周期等待，测停止是否能唤醒它。
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // 卡住真实输出使 worker 无法消费第二批，稳定覆盖满队列和停止时的尾批次。
        if (result.workerEntered) {
            for (const auto& sample : pending) reporter->submit(sample);
        }
        if (stopMode == StopMode::DuringOutput) {
            std::promise<void> started;
            auto destructionStarted = started.get_future();
            auto stopping = std::async(std::launch::async, [&] {
                started.set_value();
                reporter.reset();
            });
            destructionStarted.wait();
            // 析构与 worker 输出真实并发；先确认析构仍在等待，不靠「release 后马上析构」抢调度。
            stopping.wait_for(std::chrono::milliseconds(100));
            captured.releaseWorker();
            stopping.get();
        } else {
            captured.releaseWorker();
            if (stopMode == StopMode::AfterBatch && result.workerEntered) {
                result.batchReported = captured.waitForReports(2);
            }
            const auto stopStart = std::chrono::steady_clock::now();
            reporter.reset();
            result.shutdownMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - stopStart).count();
        }
    }
    std::cout.rdbuf(previous);
    result.output = captured.text();
    std::ifstream csv(dir / "engine_perf.csv");
    std::string line;
    while (std::getline(csv, line)) {
        std::istringstream fields(line);
        std::vector<double> row;
        std::string field;
        while (std::getline(fields, field, ',')) row.push_back(std::stod(field));
        result.rows.push_back(std::move(row));
    }
    csv.close();
    std::filesystem::remove_all(dir);
    return result;
}

double lastMetric(const std::string& output, const std::string& field) {
    const std::regex pattern(field + "([0-9]+(?:\\.[0-9]+)?)");
    double value = -1;
    for (std::sregex_iterator it(output.begin(), output.end(), pattern), end; it != end; ++it) {
        value = std::stod((*it)[1]);
    }
    return value;
}

pipeline::PerfStats sample(double decode, long long rss = 0) {
    pipeline::PerfStats stats;
    stats.decodeMs = decode;
    stats.rssBytes = rss;
    return stats;
}

bool require(bool condition, const char* message) {
    if (!condition) std::cerr << "[perf_reporter] " << message << std::endl;
    return condition;
}

constexpr long long kMiB = 1024 * 1024;

}  // namespace

bool test_perf_reporter_nearest_rank_and_stage_totals() {
    std::vector<pipeline::PerfStats> pending;
    for (int i = 20; i >= 1; --i) {
        auto stats = sample(i);
        stats.preMs = 2;
        stats.inferMs = 3;
        stats.postMs = 4;
        stats.renderMs = 5;
        pending.push_back(stats);
    }
    const auto result = report(sample(1000), pending);
    // 20 个总耗时为 15..34：最近秩 p50 是第 10 个(24)，p95 是第 19 个(33)。
    return require(result.workerEntered && result.batchReported, "真实 worker 未报告第二批") &&
        require(lastMetric(result.output, "Mean=") == 24.5, "Total 未包含全部阶段") &&
        require(lastMetric(result.output, "P50=") == 24, "p50 未采用最近秩") &&
        require(lastMetric(result.output, "P95=") == 33, "p95 未采用最近秩") &&
        require(lastMetric(result.output, "Max=") == 34, "最大总耗时错误") &&
        require(result.rows.size() == 21 && result.rows.back() == std::vector<double>({1, 2, 3, 4, 5, 0}),
                "CSV 未保留各阶段实测字段");
}

bool test_perf_reporter_rss_uses_batch_maximum() {
    const auto result = report(sample(1, 3 * kMiB), {sample(2, 30 * kMiB), sample(3, 7 * kMiB)});
    return require(result.workerEntered && result.batchReported, "真实 worker 未报告 RSS 批次") &&
        require(lastMetric(result.output, "Peak RSS: ") == 30, "RSS 取了末样本而非批次峰值");
}

bool test_perf_reporter_rss_preserves_peak_across_batches() {
    const auto result = report(sample(1, 40 * kMiB), {sample(2, 3 * kMiB)});
    return require(result.workerEntered && result.batchReported, "真实 worker 未报告跨批次样本") &&
        require(lastMetric(result.output, "Peak RSS: ") == 40, "跨批次采样峰值被低值覆盖");
}

bool test_perf_reporter_counts_dropped_samples() {
    std::vector<pipeline::PerfStats> pending;
    for (int i = 1; i <= 1030; ++i) pending.push_back(sample(i));
    const auto result = report(sample(0), pending);
    return require(result.workerEntered && result.batchReported, "真实 worker 未报告满队列批次") &&
        require(result.rows.size() == 1025 && result.rows[1][0] == 7 && result.rows.back()[0] == 1030,
                "有界队列未保留最新 1024 个样本") &&
        require(lastMetric(result.output, "Dropped: ") == 6, "丢弃 6 个样本未计数披露");
}

bool test_perf_reporter_shutdown_drains_pending_samples() {
    const auto result = report(sample(1), {sample(7), sample(9)}, StopMode::DuringOutput);
    return require(result.workerEntered, "真实 worker 未进入首批输出") &&
        require(result.rows.size() == 3 && result.rows[1][0] == 7 && result.rows[2][0] == 9,
                "析构丢失尚未处理的尾批次");
}

bool test_perf_reporter_shutdown_wakes_idle_worker() {
    const auto result = report(sample(1), {sample(7)}, StopMode::WhileIdle);
    return require(result.workerEntered && result.batchReported, "真实 worker 未进入周期等待前的首批") &&
        require(result.shutdownMs < 500, "析构未及时唤醒周期等待") &&
        require(result.rows.size() == 2 && result.rows.back()[0] == 7, "唤醒后未写入尾样本");
}
