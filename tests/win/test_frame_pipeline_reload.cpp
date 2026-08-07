#include "rk_win/FramePipeline.h"
#include "rk_win/WinConfig.h"

#include <atomic>
#include <chrono>
#include <mutex>
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

namespace {

// 假相机：记录每次 open 的 deviceId，readFrameBgr 返回假帧（模拟成功采集）。
// failOn 非空时，open 该 deviceId 返回失败（模拟目标设备打开失败，验证失败保持旧管线）。
class FakeCamera : public rk_win::ICameraSource {
public:
    std::mutex m;
    std::wstring lastOpenedId;
    std::wstring failOn;
    std::atomic<int> openCount{0};

    rk_win::CameraOpenResult open(const std::wstring& id, int w, int h, int fps) override {
        {
            std::lock_guard<std::mutex> lk(m);
            lastOpenedId = id;
        }
        openCount.fetch_add(1);
        rk_win::CameraOpenResult r;
        r.ok = true;
        r.code = "ok";
        {
            std::lock_guard<std::mutex> lk(m);
            if (id == failOn) r.ok = false;
        }
        return r;
    }
    void close() override {}
    rk_win::CameraOpenResult readFrameBgr(cv::Mat& out, std::uint64_t& ts) override {
        out = cv::Mat(360, 640, CV_8UC3, cv::Scalar(10, 20, 30));
        ts = 1;
        rk_win::CameraOpenResult r;
        r.ok = true;
        r.code = "ok";
        return r;
    }
    bool isOpen() const override { return true; }
    rk_win::CameraDevice currentDevice() const override { return rk_win::CameraDevice{}; }
    rk_win::CameraFormat currentFormat() const override { return rk_win::CameraFormat{}; }

    std::wstring openedId() {
        std::lock_guard<std::mutex> lk(m);
        return lastOpenedId;
    }
};

std::vector<rk_win::CameraDevice> makeFakeDevices(rk_win::CameraFormat f0) {
    rk_win::CameraDevice dev0;
    dev0.name = L"Cam0";
    dev0.deviceId = L"cam0";
    dev0.formats.push_back(f0);
    rk_win::CameraDevice dev1;
    dev1.name = L"Cam1";
    dev1.deviceId = L"cam1";
    dev1.formats.push_back(f0);
    return {dev0, dev1};
}

// 轮询直到谓词成立或超时
template <typename Pred>
bool waitUntil(Pred pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

}  // namespace

// 相机切换：注入假设备（cam0/cam1）与假相机。
// 修复前：switchCamera 仅用 CameraSession::switchWithRollback 探测（临时 MfCamera），
//   不切换 FramePipeline 的采集管线 → FakeCamera 从未被 open → lastOpenedId 为空。
// 修复后：switchCamera 原子替换采集源；startCameraWithRollbackLocked 真正等待首帧。
// 先切到 cam0 建立活动相机，再切到 cam1 → 最终活动源是 cam1（采集源确实改变）。
bool test_pipeline_switch_camera_switches_source() {
    // 不调 initialize（避免启动 processLoop/真实枚举），直接注入假设备与假相机验证切换。
    rk_win::FramePipeline fp;

    rk_win::CameraFormat f0;
    f0.width = 640;
    f0.height = 480;
    f0.fps = 30;
    fp.setDevicesForTest(makeFakeDevices(f0));

    auto fake = std::make_unique<FakeCamera>();
    auto* fakePtr = fake.get();
    fp.setCameraForTest(std::move(fake));

    rk_win::AppConfig cfg0;
    cfg0.camera.preferredDeviceId = L"cam0";
    cfg0.camera.width = 640;
    cfg0.camera.height = 480;
    cfg0.camera.fps = 30;
    fp.switchCamera(cfg0);  // 建立活动相机 cam0

    rk_win::AppConfig cfg1;
    cfg1.camera.preferredDeviceId = L"cam1";
    cfg1.camera.width = 640;
    cfg1.camera.height = 480;
    cfg1.camera.fps = 30;
    fp.switchCamera(cfg1);  // 切到 cam1：startCameraWithRollbackLocked 等首帧，成功则活动源为 cam1

    const bool switched = fakePtr->openCount.load() >= 2 && fakePtr->openedId() == L"cam1";
    fp.shutdown();
    return switched;
}

// 失败保持旧管线：cam1 open 失败 → switchCamera 失败并回滚到 cam0（旧采集源）。
// 断言回滚后 FakeCamera 重新打开的是 cam0（活动源保持旧）。
bool test_pipeline_switch_camera_failure_keeps_old() {
    rk_win::FramePipeline fp;

    rk_win::CameraFormat f0;
    f0.width = 640;
    f0.height = 480;
    f0.fps = 30;
    fp.setDevicesForTest(makeFakeDevices(f0));

    auto fake = std::make_unique<FakeCamera>();
    auto* fakePtr = fake.get();
    fake->failOn = L"cam1";  // cam1 打开失败
    fp.setCameraForTest(std::move(fake));

    rk_win::AppConfig cfg0;
    cfg0.camera.preferredDeviceId = L"cam0";
    cfg0.camera.width = 640;
    cfg0.camera.height = 480;
    cfg0.camera.fps = 30;
    fp.switchCamera(cfg0);  // 建立活动相机 cam0（成功）

    rk_win::AppConfig cfg1;
    cfg1.camera.preferredDeviceId = L"cam1";
    cfg1.camera.width = 640;
    cfg1.camera.height = 480;
    cfg1.camera.fps = 30;
    fp.switchCamera(cfg1);  // cam1 失败 → 回滚到 cam0

    // 回滚后 captureLoop 重新打开旧设备 cam0（异步），轮询等待
    const bool keptOld = waitUntil([&] { return fakePtr->openedId() == L"cam0"; }, 2000) &&
                         fakePtr->openCount.load() >= 2;
    fp.shutdown();
    return keptOld;
}
