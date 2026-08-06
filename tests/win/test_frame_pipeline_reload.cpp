#include "rk_win/FramePipeline.h"
#include "rk_win/WinConfig.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <opencv2/core.hpp>

// 并发 reload/enroll 压力测试：processLoop 消费注入帧（执行 processor->run）的同时，
// reloadRuntime 原子替换 runtime 快照并请求注册/清库。
// 修复前：processor_/recognizer_/dnn_ 为裸指针成员，reloadRuntime 直接 move 替换，
//   并发 run() 期间旧对象被销毁 → UAF（崩溃/挂起）。
// 修复后：shared_ptr 快照共享所有权，processLoop 持有快照整个 run()，无 UAF。
bool test_pipeline_concurrent_reload_no_uaf() {
    rk_win::AppConfig cfg;
    cfg.model.recognition = "lbph";
    cfg.recognition.cascadePath = "assets/lbpcascade_frontalface.xml";
    cfg.recognition.databasePath = ".";
    cfg.dnn.enable = false;

    rk_win::FramePipeline fp;
    if (!fp.initialize(cfg)) {
        return false;
    }

    std::atomic<bool> stop{false};
    std::atomic<int> reloadCount{0};

    // 帧生产者：驱动 processLoop 持续执行 processor->run()
    std::thread producer([&fp, &stop] {
        cv::Mat frame(360, 640, CV_8UC3, cv::Scalar(64, 128, 192));
        while (!stop.load(std::memory_order_relaxed)) {
            fp.injectFrameForTest(frame);
        }
    });

    // 并发 reload/enroll 线程：持续替换 runtime 快照并请求注册/清库
    std::thread reloader([&fp, &stop, &reloadCount, &cfg] {
        int i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            fp.reloadRuntime(cfg);
            fp.requestEnroll("并发注册-" + std::to_string(i++));
            fp.requestClearDb();
            reloadCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    reloader.join();

    // reload 确实发生（排除空转假绿）
    if (reloadCount.load(std::memory_order_relaxed) < 20) {
        return false;
    }

    fp.shutdown();
    return true;
}
