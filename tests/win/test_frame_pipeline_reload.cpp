#include "rk_win/FramePipeline.h"
#include "rk_win/IRecognizer.h"
#include "rk_win/WinConfig.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
// 可配置场景：failOn/failOpen（打开失败）、delayFrames（前 N 帧失败延迟首帧）、neverFrame（首帧永不到）。
class FakeCamera : public rk_win::ICameraSource {
public:
    std::mutex m;
    std::wstring lastOpenedId;
    std::wstring failOn;
    std::atomic<int> openCount{0};
    int delayFrames = 0;      // readFrameBgr 前 N 次失败，之后成功（延迟首帧）
    bool failOpen = false;    // open 返回失败
    bool neverFrame = false;  // readFrameBgr 永远失败
    std::atomic<int> readCount{0};

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
            // 打开失败分支需同步置 code：仅置 ok=false 会让 reason 呈现"摄像头打开失败: ok"（误导观感）
            if (id == failOn || failOpen) {
                r.ok = false;
                r.code = "fail";
            }
        }
        return r;
    }
    void close() override {}
    rk_win::CameraOpenResult readFrameBgr(cv::Mat& out, std::uint64_t& ts) override {
        const int n = readCount.fetch_add(1);
        rk_win::CameraOpenResult r;
        r.ok = true;
        r.code = "ok";
        if (neverFrame || n < delayFrames) {
            r.ok = false;
            r.code = "no_frame";
            return r;
        }
        out = cv::Mat(360, 640, CV_8UC3, cv::Scalar(10, 20, 30));
        ts = 1;
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

// 门控假识别器：enrollFromFrame 阻塞直到测试放行——确定性制造
// "run() 处理旧注册期间新 requestEnroll 到达"的竞态窗口（无 sleep 固化）；
// 每次调用恰好取 1 个样本（确定性递减 12→11，保证判别断言可观察）。仅测试使用。
class GateRecognizer : public rk_win::IRecognizer {
public:
    std::mutex m;
    std::condition_variable cv;
    bool entered = false;    // 已进入 enrollFromFrame（等待测试放行）
    bool release = false;    // 测试已放行
    std::string enteredPerson;
    std::atomic<int> enrollCalls{0};

    // 测试等待进入 enrollFromFrame（确认 run() 已执行到注册步骤、回写前）
    bool waitEntered(int timeoutMs) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return entered; });
    }
    // 测试放行：使阻塞中的 enrollFromFrame 返回（此后再次调用也立即通过）
    void go() {
        {
            std::lock_guard<std::mutex> lk(m);
            release = true;
        }
        cv.notify_all();
    }

    std::vector<rk_win::FaceMatch> identify(const cv::Mat&) override { return {}; }

    bool enrollFromFrame(const std::string& personId, const cv::Mat&, int, int& samplesTakenOut) override {
        {
            std::lock_guard<std::mutex> lk(m);
            enteredPerson = personId;
            entered = true;
        }
        cv.notify_all();
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return release; });
        }
        samplesTakenOut = 1;  // 恰好取 1 个样本
        enrollCalls.fetch_add(1);
        return true;
    }
    bool saveDb() const override { return true; }
    void clearDb() override {}
    double threshold() const override { return 0.0; }
    void setThreshold(double) override {}
    std::vector<std::string> personIds() const override { return {}; }
};

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

// 并发读写 RenderState：injectFrameForTest 驱动 processLoop 写 render_（含 status），
// requestEnroll 使 processLoop 写变化状态文本，并发线程 tryGetRenderState 锁内读。
// 修复前：processLoop（773/777/780）与 captureLoop（585/591/607）在 renderMu_ 外无锁写
//   render_.status（std::string），与读线程锁内读并发 → data race（撕裂/崩溃）。
bool test_pipeline_render_state_concurrent_access() {
    rk_win::AppConfig cfg;
    cfg.model.recognition = "lbph";
    cfg.recognition.cascadePath = "assets/lbpcascade_frontalface.xml";
    cfg.recognition.databasePath = ".";
    cfg.dnn.enable = false;

    rk_win::FramePipeline fp;
    if (!fp.initialize(cfg)) return false;

    std::atomic<bool> stop{false};
    std::atomic<int> badReads{0};
    std::atomic<int> totalReads{0};

    // 帧生产者：驱动 processLoop 持续执行 processor->run()（sink publish 写 render_.bgr）
    std::thread producer([&fp, &stop] {
        cv::Mat frame(360, 640, CV_8UC3, cv::Scalar(64, 128, 192));
        while (!stop.load(std::memory_order_relaxed)) {
            fp.injectFrameForTest(frame);
        }
    });

    // 注册请求：长 personId 使 processLoop 写超长状态文本（超 SSO → 堆分配，撕裂可观察）
    const std::string longId(512, 'E');
    std::thread enroller([&fp, &stop, &longId] {
        while (!stop.load(std::memory_order_relaxed)) {
            fp.requestEnroll(longId);
        }
    });

    // 读线程：锁内拷贝 render_ 快照，断言 status 是完整合法值
    std::thread reader([&fp, &stop, &badReads, &totalReads] {
        while (!stop.load(std::memory_order_relaxed)) {
            rk_win::RenderState st;
            if (fp.tryGetRenderState(st)) {
                totalReads.fetch_add(1, std::memory_order_relaxed);
                const std::string& s = st.status;
                const bool valid = s == "摄像头已打开" || s == "已清空人脸库" ||
                                   s.rfind("注册中: ", 0) == 0 || s.rfind("注册完成: ", 0) == 0;
                if (!valid) badReads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    enroller.join();
    reader.join();
    fp.shutdown();

    return totalReads.load(std::memory_order_relaxed) > 0 && badReads.load(std::memory_order_relaxed) == 0;
}

namespace {

// 相机等待谓词三用例：用 applyCameraSettings 返回验证 wait_for 谓词（cameraSignal_==4/3）与超时正确。
// 测试不 sleep 固定时长等结果，断言基于 startCameraWithRollbackLocked 的返回（wait_for 等待/超时）。

rk_win::CameraSwitchResult runCameraOpen(int deviceIndex, int maxTotalMs, FakeCamera* fake, rk_win::FramePipeline& fp) {
    rk_win::AppConfig cfg;
    cfg.model.recognition = "lbph";
    cfg.recognition.cascadePath = "assets/lbpcascade_frontalface.xml";
    cfg.recognition.databasePath = ".";
    cfg.dnn.enable = false;
    // initialize 只启动 processLoop（running_=true，FramePipeline.cpp:103）；
    // captureLoop 读帧循环由下方 applyCameraSettings → startCameraWithRollbackLocked 异步启动，否则首帧永不达
    fp.initialize(cfg);
    rk_win::CameraFormat f0;
    f0.width = 640;
    f0.height = 480;
    f0.fps = 30;
    fp.setDevicesForTest(makeFakeDevices(f0));
    fp.setCameraForTest(std::unique_ptr<FakeCamera>(fake));
    return fp.applyCameraSettings(deviceIndex, 640, 480, 30, maxTotalMs);
}

}  // namespace

// 延迟首帧：FakeCamera 前 5 帧失败（captureLoop 读失败各 sleep 100ms → 首帧延迟 ~500ms），
// wait_for 在 maxTotalMs(3000) 内等到首帧 → ok=true（首帧延迟但成功）。
bool test_pipeline_first_frame_delayed() {
    rk_win::FramePipeline fp;
    auto* fake = new FakeCamera();
    fake->delayFrames = 5;
    const auto res = runCameraOpen(0, 3000, fake, fp);
    fp.shutdown();
    return res.ok;
}

// 超时：readFrameBgr 永远失败 → wait_for(500) 超时 → ok=false 且原因含"超时"。
bool test_pipeline_first_frame_timeout() {
    rk_win::FramePipeline fp;
    auto* fake = new FakeCamera();
    fake->neverFrame = true;
    const auto res = runCameraOpen(0, 500, fake, fp);
    fp.shutdown();
    return !res.ok && res.reason.find("超时") != std::string::npos;
}

// 打开失败：open 返回失败 → cameraSignal_==3 谓词满足 → wait_for 立即返回 → ok=false 且原因含"打开失败"。
bool test_pipeline_open_failure() {
    rk_win::FramePipeline fp;
    auto* fake = new FakeCamera();
    fake->failOpen = true;
    const auto res = runCameraOpen(0, 500, fake, fp);
    fp.shutdown();
    return !res.ok && res.reason.find("打开失败") != std::string::npos;
}

// enroll 回写竞态（B06-018 残余，本工作单元 RW3 修复）：处理旧注册（personA）期间
// 新 requestEnroll("personB") 到达 → 旧命令回写不得覆盖 B 的样本计数。
// 时序（确定性，无 sleep 固化）：门控假识别器在 run() 注册步骤阻塞 → 测试确认进入后
// 发起新注册 → 放行 → 等该帧处理完成（facesSeq 递增发生在回写之后）→ 断言剩余计数。
// 修复前：回写无条件写 enrollRemaining_=旧命令 post-run 值（12→11），B 的 12 被覆盖 → 断言失败（RED）。
// 修复后：epoch 守卫（cmd epoch=1 vs 当前 enrollEpoch_=2 不等）→ 跳过覆盖，12 保持 → 断言通过（GREEN）。
bool test_pipeline_enroll_writeback_does_not_overwrite_new_request() {
    rk_win::AppConfig cfg;
    cfg.model.recognition = "lbph";
    cfg.recognition.cascadePath = "assets/lbpcascade_frontalface.xml";
    cfg.recognition.databasePath = ".";
    cfg.recognition.enrollSamples = 12;
    cfg.dnn.enable = false;

    rk_win::FramePipeline fp;
    if (!fp.initialize(cfg)) return false;

    // 注入门控假识别器：此刻 processLoop 空闲（尚无帧注入 → 阻塞在 frameCv_）
    auto gate = std::make_shared<GateRecognizer>();
    fp.setRecognizerForTest(gate);

    // 1. 发起旧注册 personA：enrollEpoch_→1，remaining=12
    fp.requestEnroll("personA");

    // 2. 注入一帧驱动 processLoop：build cmd(personA, remaining=12, epoch=1) →
    //    run() 进入 enrollFromFrame 阻塞（门控），制造"处理中途"确定性窗口
    const std::uint64_t seqBefore = fp.currentFacesSeq();
    cv::Mat frame(360, 640, CV_8UC3, cv::Scalar(64, 128, 192));
    fp.injectFrameForTest(frame);

    // 3. 确认 run() 已进入注册步骤且处理的是 personA（而非后到的 personB）
    // 早退先放行门控再 shutdown，避免 join 卡死（RW3 遗留）：一旦门控已阻塞在
    // enrollFromFrame（entered=true 且未放行），直接 shutdown 会 join 该门控线程而挂死；
    // 兜底 gate->go() 确保门控不再阻塞，shutdown 才能正常 join 收尾。
    if (!gate->waitEntered(2000)) { gate->go(); fp.shutdown(); return false; }
    if (gate->enteredPerson != "personA") { gate->go(); fp.shutdown(); return false; }

    // 4. 处理中途发起新注册 personB：enrollEpoch_→2，remaining=12，pending=true
    fp.requestEnroll("personB");

    // 5. 放行门控：run() 返回（旧 cmd remaining 12→11, epoch=1）→ 随后进入回写
    gate->go();

    // 6. 等待该帧处理完成（facesSeq 递增在回写之后，确保读到的计数是回写后的稳定值）
    // 早退先放行门控再 shutdown，避免 join 卡死（RW3 遗留）：兜底 gate->go() 与步骤 5
    // 放行幂等，确保门控不再阻塞后 shutdown 才能正常 join。
    std::uint64_t seqAfter = 0;
    if (!fp.waitFacesSeqChanged(seqBefore, 2000, seqAfter)) { gate->go(); fp.shutdown(); return false; }

    // 7. 判别断言：新请求 personB 的剩余计数必须保持 12（未被旧命令的 11 覆盖）
    const bool preserved = fp.enrollRemainingForTest() == cfg.recognition.enrollSamples;

    // 8. 收尾：兜底放行（防理论上再次进入门控的帧卡死）后关闭
    gate->go();
    fp.shutdown();
    return preserved;
}
