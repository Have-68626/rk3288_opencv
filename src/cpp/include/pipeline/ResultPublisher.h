#pragma once
#include <functional>
#include <string>
#include <mutex>
#include <vector>
#if __has_include(<opencv2/core.hpp>) && !defined(RK_SKIP_OPENCV)
#include <opencv2/core.hpp>
#else
namespace cv { class Mat; }
#endif
#include "pipeline/TrackCoordinator.h"  // TrackView

namespace pipeline {

struct DomainEvent {
    std::string type;       // "VERIFIED" | "AUTH_FAIL" | "NO_FACE" | "FACES"
    std::string message;
    long long timestampMs = 0;
};

struct PerfStats {
    double decodeMs = 0.0;
    double preMs = 0.0;
    double inferMs = 0.0;
    double postMs = 0.0;
    double renderMs = 0.0;
    long long rssBytes = 0;
};

struct FrameOutcome {
    cv::Mat renderFrame;
    std::vector<TrackView> tracks;
    std::vector<DomainEvent> events;
    PerfStats stats;
};

class ResultPublisher {
public:
    using TextCallback = std::function<void(const std::string&)>;

    ResultPublisher();

    // 设置回调（JNI/Java 层注册）
    void setCallback(TextCallback cb);

    // 发布一帧的副作用：回调节流 + 渲染帧持久化 + 异常事件生产
    void publish(const FrameOutcome& outcome);

    // 获取最新渲染帧（线程安全）
    bool getRenderFrame(cv::Mat& out, uint64_t& seq);

private:
    TextCallback callback_;

    // 节流计时器
    long long lastNoFaceMs_ = 0;
    long long lastUnknownMs_ = 0;
    long long lastVerifiedMs_ = 0;
    long long lastMultiMs_ = 0;

    // 渲染帧
    cv::Mat renderFrame_;
    uint64_t renderSeq_ = 0;
    std::mutex renderMu_;
};

}  // namespace pipeline
