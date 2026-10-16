#include "rk_win/FramePipeline.h"
#include "rk_win/IRecognizer.h"
#include "rk_win/WinConfig.h"
#include "rk_win/FrameProcessor.h"
#include "rk_win/SideEffectSink.h"
#include "rk_win/FaceDatabase.h"
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
#include "../cpp/ncnn_test_models.h"
#endif
#include "FileHash.h"  // rk_wcfr::calculateSHA256：断言捕获日志中 hash 与临时文件实际值一致

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <io.h>  // MSVC: _dup/_dup2/_close/_fileno（stderr 重定向捕获）
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <opencv2/core.hpp>

namespace rk_win {
struct FramePipelineTestAccess {
    static bool databaseMutationHeld(FramePipeline& pipeline) {
        if (!pipeline.databaseMutationMu_.try_lock()) return true;
        pipeline.databaseMutationMu_.unlock();
        return false;
    }
    static std::unique_lock<std::mutex> lockPublication(FramePipeline& pipeline) {
        return std::unique_lock<std::mutex>(pipeline.publicationMu_);
    }
    static bool lifecycleHeld(FramePipeline& pipeline) {
        if (!pipeline.lifecycleMu_.try_lock()) return true;
        pipeline.lifecycleMu_.unlock();
        return false;
    }
    static bool running(FramePipeline& pipeline) { return pipeline.running_.load(); }
    // 确定性时序观测（仅测试）：确认 processLoop 已把注入帧取走。processLoop 取帧与"捕获 rt/命令"
    // 之间没有任何阻塞点（取帧 → 统计 → 命令快照 → 取数据库锁），故一旦取走即已捕获命令。
    static bool frameConsumed(FramePipeline& pipeline) {
        std::lock_guard<std::mutex> lock(pipeline.frameMu_);
        return !pipeline.hasFrame_;
    }
    static std::unique_lock<std::mutex> lockConfig(FramePipeline& pipeline) {
        return std::unique_lock<std::mutex>(pipeline.mu_);
    }
    static std::shared_ptr<void> runtime(FramePipeline& pipeline) {
        std::lock_guard<std::mutex> lock(pipeline.runtimeMu_);
        return pipeline.runtime_;
    }
    static void setCallback(FramePipeline& pipeline, SideEffectSink::FacesCallback callback) {
        pipeline.sink_->setFacesCallback(std::move(callback));
    }
};
}

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

namespace {

// Q4-4 临时文件助手：cascade A/B 两个内容不同但均可合法加载的模型副本 + stderr 捕获日志，
// 析构时清理。用进程内递增序号保证多次运行/并行构建目录互不冲突。
struct ManifestTempFiles {
    std::filesystem::path fileA;
    std::filesystem::path fileB;
    std::filesystem::path capture;
    ~ManifestTempFiles() {
        std::error_code ec;
        std::filesystem::remove(fileA, ec);
        std::filesystem::remove(fileB, ec);
        std::filesystem::remove(capture, ec);
    }
};

ManifestTempFiles makeManifestTempFiles() {
    static int seq = 0;
    const int n = ++seq;
    const auto tmp = std::filesystem::temp_directory_path();
    ManifestTempFiles f;
    f.fileA = tmp / ("rk_wcfr_q44_cascade_a_" + std::to_string(n) + ".xml");
    f.fileB = tmp / ("rk_wcfr_q44_cascade_b_" + std::to_string(n) + ".xml");
    f.capture = tmp / ("rk_wcfr_q44_manifest_capture_" + std::to_string(n) + ".log");
    return f;
}

// 定位真实 LBP cascade（与 R2-3 测试同一探测策略）。两配置的 cascade 必须可被
// cv::CascadeClassifier 正常加载：空/非法 XML 会落入 haar_cvt::convert 旧格式转换
// 路径而崩溃（Q4-4 实证），故以"真实 cascade + 注入注释"构造两个合法、hash 不同的模型。
std::filesystem::path findRealCascade() {
    const std::filesystem::path cwd = std::filesystem::current_path();
    for (const auto& cand : {
             std::filesystem::path(RK_TEST_SOURCE_ROOT) / "app" / "src" / "main" / "assets" / "lbpcascade_frontalface.xml",
             cwd / "assets" / "lbpcascade_frontalface.xml",
             cwd.parent_path() / "app" / "src" / "main" / "assets" / "lbpcascade_frontalface.xml",
             cwd / "app" / "src" / "main" / "assets" / "lbpcascade_frontalface.xml"}) {
        if (std::filesystem::exists(cand)) return cand;
    }
    return {};
}

// 从捕获的 stderr 中提取 cascade_frontalface 的 MODEL_MANIFEST_CHECK 行。
// 过滤掉 FaceRecognizer 的 [Self-Check] 等其它 stderr 输出，只统计 manifest 校验行。
std::vector<std::string> extractCascadeManifestLines(const std::string& log) {
    const std::string marker = "MODEL_MANIFEST_CHECK [id=cascade_frontalface]";
    std::vector<std::string> out;
    std::istringstream iss(log);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find(marker) != std::string::npos) out.push_back(line);
    }
    return out;
}

}  // namespace

// Q4-4（P2）：reloadRuntime 成功后须重跑 manifest 校验，使日志反映重载后的模型 hash。
// 判别：捕获 stderr 下依次调用 initialize(cfgA) + reloadRuntime(cfgB)；cfgA/cfgB 的
//   cascade 分别为"真实 cascade 副本"与"副本注入注释"→ hashA != hashB 且均可加载。
// 修复前：reloadRuntime 不重跑校验 → 仅 initialize 打印 1 行 cascade MODEL_MANIFEST_CHECK
//   （反映 hashA），行数 < 2 且末行不反映 hashB → 断言失败（RED）。
// 修复后：reloadRuntime 重跑校验 → 恰好 2 行，首行 hashA、末行 hashB → 断言通过（GREEN）。
bool test_pipeline_reload_reruns_manifest_check() {
    auto tmp = makeManifestTempFiles();
    const std::filesystem::path real = findRealCascade();
    if (real.empty()) return false;  // 前置缺失：明确失败而非假绿
    {
        // fileA = 真实 cascade 逐字节副本；fileB = 副本在根元素后注入注释（仍合法可加载，
        // 但字节不同 → hashB != hashA）。避开空 XML 触发 haar_cvt::convert 崩溃路径。
        std::ifstream src(real, std::ios::binary);
        std::ostringstream content;
        content << src.rdbuf();
        const std::string body = content.str();
        const std::string rootTag = "<opencv_storage>";
        const size_t rootPos = body.find(rootTag);
        if (rootPos == std::string::npos) return false;  // 结构异常：不应发生
        {
            std::ofstream a(tmp.fileA, std::ios::binary);
            a << body;
        }
        {
            std::ofstream b(tmp.fileB, std::ios::binary);
            // 根元素后插注释（XML 注释，CascadeClassifier 正常忽略）→ 仅 hash 变化
            b << body.substr(0, rootPos + rootTag.size())
              << "\n<!-- q44 reload manifest variant -->\n"
              << body.substr(rootPos + rootTag.size());
        }
    }

    // 重定向 stderr 到捕获文件（manifest 校验打印目标）。MSVC fd 三连：
    // _dup 保留原 stderr fd → freopen 指向捕获文件 → 结束后 _dup2 还原（可被 ctest 重定向）。
    fflush(stderr);
    std::cerr.flush();
    const int savedFd = _dup(_fileno(stderr));
    if (savedFd < 0) return false;
    FILE* cap = freopen(tmp.capture.string().c_str(), "w", stderr);
    if (!cap) {
        _close(savedFd);
        return false;
    }

    rk_win::AppConfig cfgA;
    cfgA.model.recognition = "lbph";
    cfgA.recognition.cascadePath = tmp.fileA;
    // 目录不是有效数据库文件，成功重载的前提必须是合法候选配置。
    cfgA.recognition.databasePath = tmp.fileA.string() + ".db";
    cfgA.dnn.enable = false;

    rk_win::AppConfig cfgB = cfgA;
    cfgB.recognition.cascadePath = tmp.fileB;  // 重载后模型内容变化 → hash 不同

    bool initOk = false, reloadOk = false;
    {
        rk_win::FramePipeline fp;
        initOk = fp.initialize(cfgA);
        reloadOk = fp.reloadRuntime(cfgB);  // 重载：activeModels_ 替换为 hashB 对应模型
        fflush(stderr);
        std::cerr.flush();
        _dup2(savedFd, _fileno(stderr));  // 还原 stderr（shutdown 前），后续输出不再进捕获文件
        _close(savedFd);
        fp.shutdown();
    }
    if (!initOk || !reloadOk) return false;

    std::ifstream in(tmp.capture, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    const std::string log = oss.str();

    const auto lines = extractCascadeManifestLines(log);
    if (lines.size() != 2) return false;

    const std::string shaA = rk_wcfr::calculateSHA256(tmp.fileA);
    const std::string shaB = rk_wcfr::calculateSHA256(tmp.fileB);
    if (shaA == shaB) return false;  // 防御：内容不同则 hash 必不同
    // 首行反映 initialize 的 hashA；末行必须反映重载后的 hashB（reload 重跑了校验）
    return lines.front().find(shaA) != std::string::npos &&
           lines.back().find(shaB) != std::string::npos;
}

namespace {
struct RuntimeTestStorage {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("rk_pipeline_audit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    RuntimeTestStorage() { std::filesystem::create_directories(dir); }
    ~RuntimeTestStorage() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
    rk_win::AppConfig config() const {
        rk_win::AppConfig cfg;
        cfg.model.recognition = "lbph";
        cfg.recognition.cascadePath = std::filesystem::path(RK_TEST_SOURCE_ROOT) /
            "app/src/main/assets/lbpcascade_frontalface.xml";
        cfg.recognition.databasePath = dir / "faces.db";
        cfg.log.logDir = dir / "logs";
        cfg.recognition.enrollSamples = 7;
        cfg.dnn.enable = false;
        return cfg;
    }
};

class TimedRecognizer : public rk_win::IRecognizer {
public:
    std::atomic<int> calls{0};
    std::vector<rk_win::FaceMatch> identify(const cv::Mat&) override {
        const int call = calls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(call == 0 ? 25 : 2));
        rk_win::FaceMatch match;
        match.rect = {4, 4, 12, 12};
        return {match};
    }
    bool enrollFromFrame(const std::string&, const cv::Mat&, int, int& taken) override { taken = 0; return false; }
    bool saveDb() const override { return true; }
    void clearDb() override {}
    double threshold() const override { return 0.0; }
    void setThreshold(double) override {}
    std::vector<std::string> personIds() const override { return {}; }
};

class CloseGateCamera : public FakeCamera {
public:
    std::mutex gateMu;
    std::condition_variable gateCv;
    bool closing = false;
    bool released = false;
    std::atomic<int> closes{0};
    void close() override {
        if (closes.fetch_add(1) != 0) return;
        std::unique_lock<std::mutex> lock(gateMu);
        closing = true;
        gateCv.notify_all();
        gateCv.wait(lock, [&] { return released; });
    }
    bool waitClosing() {
        std::unique_lock<std::mutex> lock(gateMu);
        return gateCv.wait_for(lock, std::chrono::seconds(2), [&] { return closing; });
    }
    void release() {
        { std::lock_guard<std::mutex> lock(gateMu); released = true; }
        gateCv.notify_all();
    }
};
}

bool test_pipeline_processor_metrics_match_identify() {
    TimedRecognizer recognizer;
    rk_win::FrameProcessor processor(&recognizer);
    rk_win::ControlCommand command;
    command.detectStride = 8;
    cv::Mat frame(32, 32, CV_8UC3, cv::Scalar(64, 64, 64));
    const auto first = processor.run(frame.clone(), command);
    const auto second = processor.run(frame.clone(), command);
    return recognizer.calls == 2 && first.matches.size() == 1 && second.matches.size() == 1 &&
        first.inferMs >= 15.0 && second.inferMs > 0.0 && second.inferMs < first.inferMs &&
        first.consumedCommand.detectStride == 1 && second.consumedCommand.detectStride == 1;
}

bool test_pipeline_failed_reload_preserves_runtime() {
    RuntimeTestStorage storage;
    auto cfg = storage.config();
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(cfg) || pipeline.degraded().active) return false;
    auto recognizer = std::make_shared<TimedRecognizer>();
    pipeline.setRecognizerForTest(recognizer);
    const auto before = rk_win::FramePipelineTestAccess::runtime(pipeline);
    const auto models = pipeline.getActiveModels();
    cfg.recognition.cascadePath = storage.dir / "missing-cascade.xml";
    cfg.recognition.enrollSamples = 321;
    const bool reloaded = pipeline.reloadRuntime(cfg);
    pipeline.requestEnroll("测试标记");
    const auto after = rk_win::FramePipelineTestAccess::runtime(pipeline);
    const auto currentModels = pipeline.getActiveModels();
    const auto seq = pipeline.currentFacesSeq();
    pipeline.injectFrameForTest(cv::Mat(32, 32, CV_8UC3, cv::Scalar(64, 64, 64)));
    std::uint64_t done = 0;
    const bool processed = pipeline.waitFacesSeqChanged(seq, 2000, done);
    const bool preserved = !reloaded && !pipeline.runtimeReloadWarning().empty() &&
        before == after && models.size() == currentModels.size() &&
        !models.empty() && models.front().hash == currentModels.front().hash &&
        !pipeline.degraded().active && pipeline.enrollRemainingForTest() == 7 &&
        recognizer->calls == 1 && pipeline.runtimeReloadGen() == 1;
    const bool recovered = pipeline.reloadRuntime(storage.config()) &&
        pipeline.runtimeReloadWarning().empty() && pipeline.runtimeReloadGen() == 2;
    pipeline.shutdown();
    return processed && preserved && recovered;
}

bool test_pipeline_reload_config_commit_uses_reader_mutex() {
    RuntimeTestStorage storage;
    auto cfg = storage.config();
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(cfg) || pipeline.degraded().active) return false;
    cfg.recognition.enrollSamples = 321;
    auto held = rk_win::FramePipelineTestAccess::lockConfig(pipeline);
    std::atomic<bool> started{false}, finished{false};
    std::thread reload([&] { started = true; pipeline.reloadRuntime(cfg); finished = true; });
    const bool attempted = waitUntil([&] { return started.load(); }, 2000);
    const bool committedWhileHeld = waitUntil([&] { return finished.load(); }, 300);
    held.unlock();
    reload.join();
    pipeline.requestEnroll("测试标记");
    const bool applied = pipeline.enrollRemainingForTest() == 321;
    pipeline.shutdown();
    return attempted && !committedWhileHeld && applied;
}

bool test_pipeline_shutdown_waits_for_camera_change() {
    RuntimeTestStorage storage;
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(storage.config())) return false;
    pipeline.setRecognizerForTest(std::make_shared<TimedRecognizer>());
    rk_win::CameraFormat format;
    format.width = 640; format.height = 480; format.fps = 30;
    pipeline.setDevicesForTest(makeFakeDevices(format));
    auto fake = std::make_unique<CloseGateCamera>();
    auto* camera = fake.get();
    pipeline.setCameraForTest(std::move(fake));
    std::thread change([&] { pipeline.applyCameraSettings(0, 640, 480, 30, 500); });
    const bool entered = camera->waitClosing();
    std::atomic<bool> shutdownStarted{false}, shutdownFinished{false};
    std::thread stop([&] { shutdownStarted = true; pipeline.shutdown(); shutdownFinished = true; });
    const bool attempted = waitUntil([&] { return shutdownStarted.load(); }, 2000);
    const bool stoppedWhileChanging = waitUntil([&] { return shutdownFinished.load(); }, 200);
    camera->release();
    change.join(); stop.join();
    pipeline.shutdown();
    return entered && attempted && !stoppedWhileChanging;
}

bool test_pipeline_initialize_rejects_live_threads() {
    RuntimeTestStorage storage;
    rk_win::FramePipeline pipeline;
    const auto cfg = storage.config();
    if (!pipeline.initialize(cfg)) return false;
    // 覆盖 joinable thread 的赋值会 terminate，重复初始化必须在修改资源前拒绝。
    const bool rejected = !pipeline.initialize(cfg);
    pipeline.shutdown();
    return rejected;
}

bool test_pipeline_callback_reentry_does_not_deadlock() {
    RuntimeTestStorage storage;
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(storage.config())) return false;
    pipeline.setRecognizerForTest(std::make_shared<TimedRecognizer>());
    std::atomic<bool> readBack{false}, readerOk{false}, callbackDone{false}, readerDone{false};
    std::thread reader;
    rk_win::FramePipelineTestAccess::setCallback(pipeline, [&](const auto&) {
        // 读者若被回调持有的 renderMu_ 阻塞，超时后让回调返回，保证旧代码也能安全退出。
        reader = std::thread([&] {
            rk_win::RenderState render;
            rk_win::FacesSnapshot faces;
            readerOk = pipeline.tryGetRenderState(render) && pipeline.snapshotFaces(faces);
            readerDone = true;
        });
        const bool availableDuringCallback = waitUntil([&] { return readerDone.load(); }, 300);
        readBack = availableDuringCallback && readerOk.load();
        callbackDone = true;
    });
    pipeline.injectFrameForTest(cv::Mat(32, 32, CV_8UC3, cv::Scalar(64, 64, 64)));
    const bool completed = waitUntil([&] { return callbackDone.load(); }, 2000);
    if (reader.joinable()) reader.join();
    const bool unlockedDuringCallback = readBack.load();
    pipeline.shutdown();
    return completed && unlockedDuringCallback;
}

namespace {
class RuntimeCommandRecognizer final : public rk_win::IRecognizer {
public:
    std::atomic<int> enrolls{0}, clears{0}, identifies{0};
    std::vector<rk_win::FaceMatch> identify(const cv::Mat&) override { ++identifies; return {}; }
    bool enrollFromFrame(const std::string&, const cv::Mat&, int, int& taken) override { ++enrolls; taken = 1; return true; }
    bool saveDb() const override { return true; }
    void clearDb() override { ++clears; }
    double threshold() const override { return 0.0; }
    void setThreshold(double) override {}
    std::vector<std::string> personIds() const override { return {}; }
};
bool finishRuntimeCommandFrame(rk_win::FramePipeline& pipeline) {
    const auto before = pipeline.currentFacesSeq();
    pipeline.injectFrameForTest(cv::Mat(32, 32, CV_8UC3, cv::Scalar(64, 64, 64)));
    std::uint64_t after = 0;
    return pipeline.waitFacesSeqChanged(before, 2000, after);
}
}

// 模型运行时更换后，旧登记请求不得自动在新库继续采样。
bool test_pipeline_reload_cancels_pending_enrollment() {
    RuntimeTestStorage storage;
    auto cfg = storage.config();
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(cfg)) return false;
    pipeline.requestEnroll("synthetic_old_runtime");
    const bool reloaded = pipeline.reloadRuntime(cfg);
    auto current = std::make_shared<RuntimeCommandRecognizer>();
    pipeline.setRecognizerForTest(current);
    const bool processed = finishRuntimeCommandFrame(pipeline);
    const bool ok = reloaded && processed && current->enrolls == 0 && pipeline.enrollRemainingForTest() == 0;
    pipeline.shutdown();
    return ok;
}

// 清库操作只对请求时的运行时有效，不能越过切换清空另一空间。
bool test_pipeline_reload_cancels_pending_clear() {
    RuntimeTestStorage storage;
    auto cfg = storage.config();
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(cfg)) return false;
    pipeline.requestClearDb();
    const bool reloaded = pipeline.reloadRuntime(cfg);
    auto current = std::make_shared<RuntimeCommandRecognizer>();
    pipeline.setRecognizerForTest(current);
    const bool processed = finishRuntimeCommandFrame(pipeline);
    const bool ok = reloaded && processed && current->clears == 0;
    pipeline.shutdown();
    return ok;
}

// 已进入旧识别器的帧可完成，但其计数回写不能复活切换时取消的登记。
bool test_pipeline_reload_discards_inflight_enrollment_writeback() {
    RuntimeTestStorage storage;
    auto cfg = storage.config();
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(cfg)) return false;
    auto old = std::make_shared<GateRecognizer>();
    pipeline.setRecognizerForTest(old);
    pipeline.requestEnroll("synthetic_old_runtime");
    const auto before = pipeline.currentFacesSeq();
    pipeline.injectFrameForTest(cv::Mat(32, 32, CV_8UC3, cv::Scalar(64, 64, 64)));
    if (!old->waitEntered(2000)) { old->go(); pipeline.shutdown(); return false; }
    // 数据库锁序引入后，reload 必须等在途登记帧的 run() 结束（该帧持 databaseMutationMu_、
    // 正被门控阻塞），故放行门控不能再排在 reload 返回之后 —— 否则双方互等。改为并发执行，
    // 并在放行前正向确认 reload 已持生命周期锁（不靠超时把"没阻塞"误判为通过）。断言不放宽。
    std::atomic<bool> reloadOk{false}, reloadDone{false};
    std::thread reloadThread([&] { reloadOk = pipeline.reloadRuntime(cfg); reloadDone = true; });
    const bool reachedLifecycle = waitUntil([&] {
        return rk_win::FramePipelineTestAccess::lifecycleHeld(pipeline);
    }, 2000);
    old->go();
    reloadThread.join();
    const bool reloaded = reachedLifecycle && reloadOk.load() && reloadDone.load();
    if (!reloaded) { pipeline.shutdown(); return false; }
    auto current = std::make_shared<RuntimeCommandRecognizer>();
    pipeline.setRecognizerForTest(current);
    pipeline.injectFrameForTest(cv::Mat(32, 32, CV_8UC3, cv::Scalar(64, 64, 64)));
    const bool newFrame = waitUntil([&] { return current->identifies.load() == 1; }, 2000);
    std::uint64_t after = 0;
    const bool processed = newFrame && pipeline.waitFacesSeqChanged(before, 2000, after);
    rk_win::RenderState render;
    const bool cancelled = pipeline.enrollRemainingForTest() == 0 && pipeline.tryGetRenderState(render) &&
        render.status == "模型已切换，登记已取消";
    pipeline.requestEnroll("synthetic_new_runtime");
    const bool newProcessed = finishRuntimeCommandFrame(pipeline);
    const bool ok = reloaded && processed && cancelled && newProcessed && current->enrolls == 1 &&
        pipeline.enrollRemainingForTest() == cfg.recognition.enrollSamples - 1;
    pipeline.shutdown();
    return ok;
}

// 装配失败没有更换运行时，原请求必须仍由旧识别器消费。
bool test_pipeline_failed_reload_keeps_pending_commands() {
    RuntimeTestStorage storage;
    auto cfg = storage.config();
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(cfg)) return false;
    auto current = std::make_shared<RuntimeCommandRecognizer>();
    pipeline.setRecognizerForTest(current);
    pipeline.requestEnroll("synthetic_old_runtime");
    pipeline.requestClearDb();
    cfg.recognition.cascadePath = storage.dir / "absent.xml";
    const bool rejected = !pipeline.reloadRuntime(cfg);
    const bool processed = finishRuntimeCommandFrame(pipeline);
    const bool ok = rejected && processed && current->enrolls == 1 && current->clears == 1 &&
        pipeline.enrollRemainingForTest() == cfg.recognition.enrollSamples - 1;
    pipeline.shutdown();
    return ok;
}

namespace {
class IdentityGateRecognizer final : public GateRecognizer {
public:
    std::vector<rk_win::FaceMatch> identify(const cv::Mat&) override {
        std::unique_lock<std::mutex> lock(m);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
        rk_win::FaceMatch match;
        match.rect = {4, 4, 12, 12};
        match.personId = "synthetic_old_space";
        match.confidence = 0.99;
        match.accepted = true;
        return {match};
    }
};
}

// 已发布的标签像素和在途旧身份都不能跨越成功模型提交，三个输出通道同口径。
bool test_pipeline_reload_discards_old_identity_outputs() {
    RuntimeTestStorage storage;
    auto cfg = storage.config();
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(cfg)) return false;
    auto published = std::make_shared<IdentityGateRecognizer>();
    published->go();
    pipeline.setRecognizerForTest(published);
    if (!finishRuntimeCommandFrame(pipeline)) { pipeline.shutdown(); return false; }
    auto old = std::make_shared<IdentityGateRecognizer>();
    pipeline.setRecognizerForTest(old);
    std::atomic<bool> callbackOld{false};
    rk_win::FramePipelineTestAccess::setCallback(pipeline, [&](const auto& matches) {
        for (const auto& match : matches) {
            if (match.personId == "synthetic_old_space" && match.isIdentified) callbackOld = true;
        }
    });
    const auto before = pipeline.currentFacesSeq();
    pipeline.injectFrameForTest(cv::Mat(32, 32, CV_8UC3, cv::Scalar(64, 64, 64)));
    if (!old->waitEntered(2000)) { old->go(); pipeline.shutdown(); return false; }
    cfg.recognition.databasePath = storage.dir / "different_space_B.db";
    const bool reloaded = pipeline.reloadRuntime(cfg);
    rk_win::FacesSnapshot initialFaces;
    rk_win::RenderState initialRender;
    const bool cleared = !pipeline.snapshotFaces(initialFaces) && !pipeline.tryGetRenderState(initialRender);
    auto current = std::make_shared<RuntimeCommandRecognizer>();
    pipeline.setRecognizerForTest(current);
    // 后续空身份帧提供确定性推进；不得拿等待旧帧超时当作丢弃成功。
    pipeline.injectFrameForTest(cv::Mat(32, 32, CV_8UC3, cv::Scalar(64, 64, 64)));
    old->go();
    const bool newProcessed = waitUntil([&] { return current->identifies.load() == 1; }, 2000);
    std::uint64_t after = 0;
    const bool notified = pipeline.waitFacesSeqChanged(before, 2000, after);
    rk_win::FacesSnapshot faces;
    rk_win::RenderState render;
    const bool clean = pipeline.snapshotFaces(faces) && faces.faces.empty() &&
        pipeline.tryGetRenderState(render) && render.faces.empty() && !callbackOld.load();
    pipeline.shutdown();
    return reloaded && cleared && newProcessed && notified && clean;
}

// 回调同步重载须明确拒绝；只读回查保持可用，不能形成 publication → lifecycle 锁环。
bool test_pipeline_callback_rejects_runtime_reload() {
    RuntimeTestStorage storage;
    auto cfg = storage.config();
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(cfg)) return false;
    pipeline.setRecognizerForTest(std::make_shared<RuntimeCommandRecognizer>());
    std::atomic<bool> done{false}, rejected{false}, readable{false};
    rk_win::FramePipelineTestAccess::setCallback(pipeline, [&](const auto&) {
        rejected = !pipeline.reloadRuntime(cfg);
        rk_win::RenderState render;
        rk_win::FacesSnapshot faces;
        readable = pipeline.tryGetRenderState(render) && pipeline.snapshotFaces(faces);
        done = true;
    });
    pipeline.injectFrameForTest(cv::Mat(32, 32, CV_8UC3, cv::Scalar(64, 64, 64)));
    const bool completed = waitUntil([&] { return done.load(); }, 2000);
    pipeline.shutdown();
    return completed && rejected && readable;
}

// reload 等待发布屏障时必须已持生命周期锁；shutdown 置停后旧计算仍可退场，不能形成三方锁环。
bool test_pipeline_reload_shutdown_and_publication_lock_order() {
    RuntimeTestStorage storage;
    auto cfg = storage.config();
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(cfg)) return false;
    auto old = std::make_shared<IdentityGateRecognizer>();
    pipeline.setRecognizerForTest(old);
    pipeline.injectFrameForTest(cv::Mat(32, 32, CV_8UC3, cv::Scalar(64, 64, 64)));
    if (!old->waitEntered(2000)) { old->go(); pipeline.shutdown(); return false; }
    auto publication = rk_win::FramePipelineTestAccess::lockPublication(pipeline);
    std::atomic<bool> reloaded{false}, stopped{false};
    std::thread reload([&] { reloaded = pipeline.reloadRuntime(cfg); });
    const bool lifecycleFirst = waitUntil([&] {
        return rk_win::FramePipelineTestAccess::lifecycleHeld(pipeline);
    }, 2000);
    if (!lifecycleFirst) {
        publication.unlock(); old->go(); reload.join(); pipeline.shutdown(); return false;
    }
    std::thread stop([&] { pipeline.shutdown(); stopped = true; });
    publication.unlock();
    const bool stopping = waitUntil([&] {
        return !rk_win::FramePipelineTestAccess::running(pipeline);
    }, 2000);
    old->go();
    reload.join(); stop.join();
    return lifecycleFirst && stopping && reloaded && stopped;
}

// 取锁后必须复核运行时仍是当前快照（此前**无判别锚**的那一行）：processLoop 先捕获 rt 与命令、
// 再阻塞在数据库锁上，若期间重载成功提交，那条已作废的命令**绝不能**再交给旧快照执行 ——
// 否则旧识别器会把被取消的登记写回同空间库（其"结果"虽会被发布屏障丢弃，但**副作用已经发生**）。
// 时序全部靠正向观测固定，不靠 sleep 抢跑：测试占住 publicationMu_ ⇒ 重载装配完成后必然卡在提交
// 门前，且这期间**持续持有** databaseMutationMu_ ⇒ 注入的帧必然被挡在这把锁上。
bool test_pipeline_stale_command_does_not_run_after_reload_wait() {
    RuntimeTestStorage storage;
    auto cfg = storage.config();
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(cfg)) return false;
    auto stale = std::make_shared<RuntimeCommandRecognizer>();
    pipeline.setRecognizerForTest(stale);

    auto publication = rk_win::FramePipelineTestAccess::lockPublication(pipeline);
    std::atomic<bool> reloadDone{false}, reloadOk{false};
    std::thread reload([&] { reloadOk = pipeline.reloadRuntime(cfg); reloadDone = true; });
    // 正向确认重载已持数据库锁（装配中，或已停在提交门前等 publicationMu_）。
    const bool dbLocked = waitUntil([&] {
        return rk_win::FramePipelineTestAccess::databaseMutationHeld(pipeline);
    }, 5000);
    if (!dbLocked) { publication.unlock(); reload.join(); pipeline.shutdown(); return false; }

    // 提交前注入登记帧：命令取到的是**重载前**的 rt 与 enroll 请求。
    const std::uint64_t seqBefore = pipeline.currentFacesSeq();
    pipeline.requestEnroll("synthetic_stale_request");
    pipeline.injectFrameForTest(cv::Mat(32, 32, CV_8UC3, cv::Scalar(64, 64, 64)));
    // 正向确认该帧已被 processLoop 取走。取帧 → 统计 → 命令快照 → 取数据库锁 这段路径上没有 sleep/IO，
    // 且此场景下 mu_/runtimeMu_ 无争用（重载正卡在 publicationMu_ 上，两把锁都不持有）⇒ 取走后随即捕获命令。
    const bool captured = waitUntil([&] {
        return rk_win::FramePipelineTestAccess::frameConsumed(pipeline);
    }, 2000);
    if (!captured) { publication.unlock(); reload.join(); pipeline.shutdown(); return false; }
    // 被挡住期间旧识别器一次都不该被调用（命令还没拿到数据库锁）。
    const bool blockedBeforeCommit = stale->enrolls.load() == 0;

    // 放行提交：重载完成并释放数据库锁，被挡住的旧命令随即拿到锁并进入复核分支。
    publication.unlock();
    reload.join();
    // 有界负向等待做判别：缺复核时旧命令会在微秒级拿到锁并真的调用旧识别器 ⇒ 该窗口内必然观察到
    // enrolls>0；有复核时它**恒不执行**（与机器快慢无关，不是"更慢"，故不会假红）。
    const bool staleRan = waitUntil([&] { return stale->enrolls.load() > 0; }, 500);
    const bool staleNeverRan = !staleRan && stale->identifies.load() == 0 &&
        reloadOk.load() && reloadDone.load();
    // 前提自检：若命令其实是在重载提交**之后**才被捕获（调度器把 processLoop 在整个取帧→捕获区间
    // 挂起），则该命令已被重载作废（enrollRequested 被清）⇒ 不取数据库锁、快照仍是当前 ⇒ 这帧会被
    // **正常发布**，facesSeq 必然前进。把它一并纳入判据，可把这种"前提不成立"从静默漏检变成响亮失败。
    const bool premiseHeld = pipeline.currentFacesSeq() == seqBefore;
    pipeline.shutdown();
    return dbLocked && captured && blockedBeforeCommit && staleNeverRan && premiseHeld;
}

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
namespace {
rk_win::AppConfig persistentProfileConfig(const RuntimeTestStorage& storage) {
    const auto dir = storage.dir / "profile";
    std::filesystem::create_directories(dir);
    if (!rk_test::writeConstantScrfdModel(dir, 320) || !rk_test::writeEmbeddingModel(dir, false)) return {};
    std::ofstream(dir / "manifest.json") <<
        "{\"schemaVersion\":1,\"profileId\":\"synthetic_persistence\",\"displayName\":\"合成持久化模型\","
        "\"supportedAbis\":[\"x86_64\"],\"templateSpaceId\":\"" << std::string(64, 'a') << "\","
        "\"detector\":{\"param\":{\"path\":\"scrfd.param\"},\"bin\":{\"path\":\"scrfd.bin\"},"
        "\"input\":\"input.1\",\"outputs\":[\"s8\",\"s16\",\"s32\",\"b8\",\"b16\",\"b32\",\"k8\",\"k16\",\"k32\"],"
        "\"inputWidth\":320,\"inputHeight\":320,\"decodeMode\":\"distance\",\"outputLayout\":\"nchw\",\"anchorsPerPos\":[2,2,2]},"
        "\"embedder\":{\"param\":{\"path\":\"embedding.param\"},\"bin\":{\"path\":\"embedding.bin\"},"
        "\"input\":\"in0\",\"output\":\"out0\",\"inputWidth\":112,\"inputHeight\":112,\"embeddingDim\":512},"
        "\"detectionConfidence\":0.55,\"nmsIou\":0.4,\"identitySimilarity\":0.73,"
        "\"modelVersion\":\"synthetic-v1\",\"licenseId\":\"test\"}";
    auto cfg = storage.config();
    cfg.model.faceProfileDir = dir;
    cfg.recognition.enrollSamples = 1;
    return cfg;
}

class PersistentGateRecognizer final : public rk_win::IRecognizer {
    std::unique_ptr<rk_win::IRecognizer> real_;
    bool gateClear_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool entered_ = false, released_ = false;
    void enter() {
        std::unique_lock<std::mutex> lock(mu_);
        entered_ = true; cv_.notify_all();
        cv_.wait(lock, [&] { return released_; });
    }
public:
    PersistentGateRecognizer(std::unique_ptr<rk_win::IRecognizer> real, bool clear)
        : real_(std::move(real)), gateClear_(clear) {}
    bool waitEntered() {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::seconds(3), [&] { return entered_; });
    }
    void go() { { std::lock_guard<std::mutex> lock(mu_); released_ = true; } cv_.notify_all(); }
    std::vector<rk_win::FaceMatch> identify(const cv::Mat& image) override { return real_->identify(image); }
    bool enrollFromFrame(const std::string& id, const cv::Mat& image, int count, int& taken) override {
        if (!gateClear_) enter();
        return real_->enrollFromFrame(id, image, count, taken);
    }
    bool saveDb() const override { return real_->saveDb(); }
    void clearDb() override { if (gateClear_) enter(); real_->clearDb(); }
    double threshold() const override { return real_->threshold(); }
    void setThreshold(double value) override { real_->setThreshold(value); }
    std::vector<std::string> personIds() const override { return real_->personIds(); }
};

bool reloadOrdersPersistentMutation(bool clear) {
    RuntimeTestStorage storage;
    auto cfg = persistentProfileConfig(storage);
    if (cfg.model.faceProfileDir.empty()) return false;
    const cv::Mat image(320, 320, CV_8UC3, cv::Scalar(80, 120, 160));
    if (clear) {
        auto seed = rk_win::RuntimeBootstrap::build(cfg);
        int taken = 0;
        if (!seed.ok || !seed.recognizer->enrollFromFrame("synthetic_to_clear", image, 1, taken) ||
            taken != 1 || !seed.recognizer->saveDb()) return false;
    }
    rk_win::FramePipeline pipeline;
    if (!pipeline.initialize(cfg)) return false;
    auto initial = rk_win::RuntimeBootstrap::build(cfg);
    if (!initial.ok) { pipeline.shutdown(); return false; }
    auto old = std::make_shared<PersistentGateRecognizer>(std::move(initial.recognizer), clear);
    pipeline.setRecognizerForTest(old);
    if (clear) pipeline.requestClearDb(); else pipeline.requestEnroll("synthetic_old_request");
    pipeline.injectFrameForTest(image);
    if (!old->waitEntered()) { old->go(); pipeline.shutdown(); return false; }
    const bool mutationProtected = rk_win::FramePipelineTestAccess::databaseMutationHeld(pipeline);
    std::atomic<bool> reloadDone{false}, reloadOk{false};
    std::thread reload([&] { reloadOk = pipeline.reloadRuntime(cfg); reloadDone = true; });
    // 有锁时正向确认重载已进入生命周期边界，再放旧保存；缺锁时先让候选读库完成，确定制造旧覆盖。
    const bool synchronized = waitUntil([&] {
        return mutationProtected ? rk_win::FramePipelineTestAccess::lifecycleHeld(pipeline) : reloadDone.load();
    }, 5000);
    // 进入生命周期边界后，重载还必须被在途数据库变更挡在候选整库 load 之前：若它能在放行旧保存
    // 之前跑完，就说明"候选 load 与在途 saveDb 互斥"不成立（候选会读到旧快照）。用有界负向等待
    // 判别，不把固定 sleep 当通过条件，也不放宽任何行为断言。
    const bool blockedByMutation = mutationProtected
        ? !waitUntil([&] { return reloadDone.load(); }, 1000) : true;
    old->go(); reload.join();
    if (!synchronized || !blockedByMutation || !reloadOk) { pipeline.shutdown(); return false; }
    const auto before = pipeline.currentFacesSeq();
    pipeline.requestEnroll("synthetic_new_request");
    pipeline.injectFrameForTest(image);
    std::uint64_t after = 0;
    const bool processed = pipeline.waitFacesSeqChanged(before, 5000, after);
    pipeline.shutdown();
    rk_win::FaceDatabase reopened;
    const auto path = storage.dir / "face_spaces" / std::string(64, 'a') / "faces.db";
    const bool loaded = reopened.load(path);
    const bool oldCorrect = clear ? !reopened.hasPerson("synthetic_to_clear") : reopened.hasPerson("synthetic_old_request");
    return mutationProtected && processed && loaded && oldCorrect && reopened.hasPerson("synthetic_new_request");
}
}
#endif

// 同空间重新加载必须先看到旧帧完成的真实加密保存，后续新保存不得覆盖旧人员。
bool test_pipeline_profile_reload_preserves_inflight_enrollment() {
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    return reloadOrdersPersistentMutation(false);
#else
    return true;
#endif
}

// 清库与候选加载也需排序，不能让候选再次保存时复活已清除人员。
bool test_pipeline_profile_reload_does_not_restore_cleared_person() {
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    return reloadOrdersPersistentMutation(true);
#else
    return true;
#endif
}
