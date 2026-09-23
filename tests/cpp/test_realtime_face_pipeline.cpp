#include "pipeline/RealtimeFacePipeline.h"

#include "FaceSearch.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using rk_core::FaceRuntimeProfile;
using rk_core::IRealtimeDetector;
using rk_core::IRealtimeEmbedder;
using rk_core::RealtimeBenchmarkConfig;
using rk_core::RealtimeFaceBox;
using rk_core::RealtimeFacePipeline;
using rk_core::RealtimePipelineHooks;
using rk_core::RealtimePipelineSnapshot;
using rk_core::RealtimeReloadResult;

// 与 test_face_runtime_profile.cpp 同构的合法 manifest v1（9 输出 + 512 维）。
std::string validManifest() {
    return
        "{\"schemaVersion\":1,"
        "\"profileId\":\"rk3288_light\",\"displayName\":\"RK3288 Light\","
        "\"supportedAbis\":[\"armeabi-v7a\",\"arm64-v8a\"],"
        "\"detector\":{\"param\":{\"path\":\"scrfd/scrfd.param\"},"
        "\"bin\":{\"path\":\"scrfd/scrfd.bin\"},"
        " \"input\":\"in0\","
        " \"outputs\":[\"out0\",\"out1\",\"out2\",\"out3\",\"out4\",\"out5\",\"out6\",\"out7\",\"out8\"],"
        " \"inputWidth\":640,\"inputHeight\":640},"
        "\"embedder\":{\"param\":{\"path\":\"arc/arc.param\"},"
        "\"bin\":{\"path\":\"arc/arc.bin\"},"
        " \"input\":\"in0\",\"output\":\"out0\",\"inputWidth\":112,\"inputHeight\":112,"
        " \"embeddingDim\":512},"
        "\"detectionConfidence\":0.55,\"nmsIou\":0.40,\"identitySimilarity\":0.45,"
        "\"modelVersion\":\"1\",\"licenseId\":\"apache-2.0\"}";
}

class FakeDetector : public IRealtimeDetector {
public:
    FakeDetector(std::string name, std::atomic<int>* calls = nullptr, int sleepUs = 0)
        : name_(std::move(name)), calls_(calls), sleepUs_(sleepUs) {}

    bool detect(const cv::Mat&, std::vector<RealtimeFaceBox>& out, std::string& err) override {
        if (calls_) calls_->fetch_add(1, std::memory_order_relaxed);
        if (sleepUs_ > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(sleepUs_));
        }
        if (!err_.empty()) {
            err = err_;
            return false;
        }
        out.push_back(RealtimeFaceBox{1.0f, 2.0f, 3.0f, 4.0f, 0.9f});
        return true;
    }
    const char* name() const override { return name_.c_str(); }

private:
    std::string name_;
    std::atomic<int>* calls_ = nullptr;
    int sleepUs_ = 0;
    std::string err_;
};

class FakeEmbedder : public IRealtimeEmbedder {
public:
    explicit FakeEmbedder(std::string name, int dim = 512)
        : name_(std::move(name)), dim_(dim) {}

    bool embed(const cv::Mat&, std::vector<float>& out, std::string& err) override {
        if (!err_.empty()) {
            err = err_;
            return false;
        }
        out.assign(static_cast<std::size_t>(dim_), 0.1f);
        return true;
    }
    int embeddingDim() const override { return dim_; }
    const char* name() const override { return name_.c_str(); }

private:
    std::string name_;
    int dim_ = 512;
    std::string err_;
};

// 可变控制块：钩子捕获 shared_ptr，测试在 reload 之间就地改配置/句柄。
struct Controls {
    std::shared_ptr<IRealtimeDetector> detector;
    std::shared_ptr<IRealtimeEmbedder> embedder;
    std::string detectorErr;  // 非空 → loadDetector 返回 nullptr 并把该值写入 err
    bool selfCheckOk = true;
    bool benchmarkOk = true;
    double p95 = 10.0;
    bool verifyOk = true;
    int selfCheckSleepUs = 0;  // >0 时自检钩子阻塞，用于制造 reloadAsync 与析构重叠
};

RealtimePipelineHooks makeMutableHooks(std::shared_ptr<Controls> ctl) {
    RealtimePipelineHooks h;
    h.verifyFiles = [ctl](const FaceRuntimeProfile&, const std::string&, std::string& err) {
        if (!ctl->verifyOk) {
            err = "model_missing";
            return false;
        }
        return true;
    };
    h.loadDetector = [ctl](const FaceRuntimeProfile&, const std::string&,
                           std::string& err) -> std::shared_ptr<IRealtimeDetector> {
        if (!ctl->detectorErr.empty()) {
            err = ctl->detectorErr;
            return nullptr;
        }
        return ctl->detector;
    };
    h.loadEmbedder = [ctl](const FaceRuntimeProfile&, const std::string&,
                           std::string&) -> std::shared_ptr<IRealtimeEmbedder> {
        return ctl->embedder;
    };
    h.loadGallery = [](const FaceRuntimeProfile&, const std::string&,
                       std::string& err) -> std::shared_ptr<const FaceSearchLinearIndex> {
        err.clear();
        return nullptr;  // 空图库合法
    };
    h.selfCheck = [ctl](const RealtimePipelineSnapshot&, std::string& err) {
        if (ctl->selfCheckSleepUs > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(ctl->selfCheckSleepUs));
        }
        if (!ctl->selfCheckOk) {
            err = "runtime_init_failed";
            return false;
        }
        return true;
    };
    h.benchmark = [ctl](const RealtimePipelineSnapshot&, const RealtimeBenchmarkConfig&,
                        double& p95, std::string& err) {
        if (!ctl->benchmarkOk) {
            err = "runtime_init_failed";
            return false;
        }
        p95 = ctl->p95;
        return true;
    };
    return h;
}

std::shared_ptr<Controls> makeControls() {
    auto ctl = std::make_shared<Controls>();
    ctl->detector = std::make_shared<FakeDetector>("detector_A");
    ctl->embedder = std::make_shared<FakeEmbedder>("embedder_A");
    return ctl;
}

}  // namespace

// 切换成功：快照有效、代次递增、句柄确实换成新对象。
TEST(RealtimeFacePipeline, ReloadSwitchesSnapshotAtomically) {
    auto ctl = makeControls();
    RealtimeFacePipeline pipe(makeMutableHooks(ctl));

    const RealtimeReloadResult r1 = pipe.reload(validManifest(), "", "arm64-v8a", "");
    ASSERT_TRUE(r1.ok) << r1.evidence;
    EXPECT_EQ(r1.reason, "ok");
    EXPECT_EQ(r1.generation, 1u);

    auto s1 = pipe.snapshot();
    ASSERT_TRUE(s1);
    EXPECT_TRUE(s1->requested);
    EXPECT_TRUE(s1->effective);
    EXPECT_EQ(s1->reason, "ok");
    EXPECT_EQ(std::string(s1->detector->name()), "detector_A");
    EXPECT_EQ(s1->profile.profileId, "rk3288_light");
    EXPECT_GT(s1->benchmarkP95Ms, 0.0);

    // 第二次重载换成 detector_B：一次替换，代次 +1，旧快照 s1 仍指向 detector_A。
    ctl->detector = std::make_shared<FakeDetector>("detector_B");
    const RealtimeReloadResult r2 = pipe.reload(validManifest(), "", "arm64-v8a", "");
    ASSERT_TRUE(r2.ok) << r2.evidence;
    EXPECT_EQ(r2.generation, 2u);
    EXPECT_EQ(pipe.generation(), 2u);
    EXPECT_EQ(std::string(pipe.snapshot()->detector->name()), "detector_B");
    EXPECT_EQ(std::string(s1->detector->name()), "detector_A");  // 旧快照未被改写
}

// 加载失败保留旧快照：detector 返回 nullptr + reason 码，旧快照原样、代次不变。
TEST(RealtimeFacePipeline, LoadFailureKeepsOldSnapshot) {
    auto ctl = makeControls();
    RealtimeFacePipeline pipe(makeMutableHooks(ctl));
    ASSERT_TRUE(pipe.reload(validManifest(), "", "arm64-v8a", "").ok);

    auto old = pipe.snapshot();
    ASSERT_TRUE(old);

    ctl->detectorErr = "missing_model";
    ctl->detector = nullptr;
    const RealtimeReloadResult r = pipe.reload(validManifest(), "", "arm64-v8a", "");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, "missing_model");
    EXPECT_EQ(r.generation, 1u);
    EXPECT_EQ(pipe.generation(), 1u);

    // 同一对象（指针相等）：原子替换未发生，旧句柄仍可用。
    EXPECT_EQ(pipe.snapshot().get(), old.get());
    EXPECT_EQ(std::string(pipe.snapshot()->detector->name()), "detector_A");
}

// reason 码透传：编译期禁用（build_disabled）不被归一成 runtime_init_failed。
TEST(RealtimeFacePipeline, BuildDisabledReasonPassThrough) {
    auto ctl = makeControls();
    ctl->detectorErr = "build_disabled";
    ctl->detector = nullptr;
    RealtimeFacePipeline pipe(makeMutableHooks(ctl));

    const RealtimeReloadResult r = pipe.reload(validManifest(), "", "arm64-v8a", "");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, "build_disabled");
    EXPECT_EQ(pipe.generation(), 0u);
}

// 基准失败保留旧快照：钩子判失败、以及超预算两种路径都不发布。
TEST(RealtimeFacePipeline, BenchmarkFailureKeepsOldSnapshot) {
    auto ctl = makeControls();
    RealtimeFacePipeline pipe(makeMutableHooks(ctl));
    ASSERT_TRUE(pipe.reload(validManifest(), "", "arm64-v8a", "").ok);
    auto old = pipe.snapshot();

    // 路径一：基准钩子返回 false
    ctl->benchmarkOk = false;
    RealtimeReloadResult r = pipe.reload(validManifest(), "", "arm64-v8a", "");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, "runtime_init_failed");
    EXPECT_EQ(pipe.snapshot().get(), old.get());

    // 路径二：基准成功但 p95 超预算（显式开启 ARM64 判定）
    ctl->benchmarkOk = true;
    ctl->p95 = 500.0;
    RealtimeBenchmarkConfig bench;
    bench.warmupIters = 10;
    bench.timedIters = 50;
    bench.p95BudgetMs = 250.0;
    bench.enforceBudget = true;
    pipe.setBenchmarkConfig(bench);

    r = pipe.reload(validManifest(), "", "arm64-v8a", "");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, "runtime_init_failed");
    EXPECT_NE(r.evidence.find("超预算"), std::string::npos);
    EXPECT_EQ(pipe.generation(), 1u);
    EXPECT_EQ(pipe.snapshot().get(), old.get());
}

// 自检失败保留旧快照。
TEST(RealtimeFacePipeline, SelfCheckFailureKeepsOldSnapshot) {
    auto ctl = makeControls();
    RealtimeFacePipeline pipe(makeMutableHooks(ctl));
    ASSERT_TRUE(pipe.reload(validManifest(), "", "arm64-v8a", "").ok);
    auto old = pipe.snapshot();

    ctl->selfCheckOk = false;
    const RealtimeReloadResult r = pipe.reload(validManifest(), "", "arm64-v8a", "");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, "runtime_init_failed");
    EXPECT_EQ(pipe.snapshot().get(), old.get());
}

// manifest 解析失败 → unsupported_input，且从未发布过快照。
TEST(RealtimeFacePipeline, ParseFailureReasonUnsupportedInput) {
    auto ctl = makeControls();
    RealtimeFacePipeline pipe(makeMutableHooks(ctl));
    const RealtimeReloadResult r = pipe.reload("{not json", "", "arm64-v8a", "");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, "unsupported_input");
    EXPECT_EQ(pipe.generation(), 0u);
    EXPECT_EQ(pipe.snapshot(), nullptr);
}

// 推理期间切换：worker 持续用当前快照推理，主线程反复 reload；旧快照须存活且代次单调。
TEST(RealtimeFacePipeline, SwitchDuringInferenceKeepsSnapshotAlive) {
    std::atomic<int> detectCalls{0};
    auto ctl = makeControls();
    ctl->detector = std::make_shared<FakeDetector>("detector_worker", &detectCalls, 20);
    RealtimeFacePipeline pipe(makeMutableHooks(ctl));
    ASSERT_TRUE(pipe.reload(validManifest(), "", "arm64-v8a", "").ok);

    constexpr int kDetects = 300;
    std::atomic<int> detectFailures{0};
    std::vector<std::uint64_t> observed(kDetects, 0);

    std::thread worker([&] {
        for (int i = 0; i < kDetects; ++i) {
            // 先原子取快照（持有期间 reload 不得销毁其句柄），再用管线读路径推理。
            auto s = pipe.snapshot();
            observed[static_cast<std::size_t>(i)] = s ? s->generation : 0;
            std::vector<RealtimeFaceBox> boxes;
            std::string err;
            if (!pipe.detect(cv::Mat(), boxes, err)) detectFailures.fetch_add(1);
        }
    });

    // 主线程在推理进行中反复原子替换（交替 detector_A/B），全部应成功。
    constexpr int kReloads = 20;
    int reloadFailures = 0;
    for (int i = 0; i < kReloads; ++i) {
        ctl->detector = std::make_shared<FakeDetector>(
            i % 2 == 0 ? "detector_B" : "detector_A", &detectCalls, 20);
        const RealtimeReloadResult r = pipe.reload(validManifest(), "", "arm64-v8a", "");
        if (!r.ok) ++reloadFailures;
    }
    worker.join();

    EXPECT_EQ(reloadFailures, 0);
    EXPECT_EQ(detectFailures.load(), 0);
    EXPECT_GT(detectCalls.load(), 0);
    EXPECT_EQ(pipe.generation(), static_cast<std::uint64_t>(kReloads + 1));
    // 代次单调不减：读取侧只会看到递增后的新快照，不会回退。
    for (std::size_t i = 1; i < observed.size(); ++i) {
        EXPECT_GE(observed[i], observed[i - 1]);
    }
}

// 后台重载：reloadAsync 完成后快照已就绪。
TEST(RealtimeFacePipeline, AsyncReloadPublishesSnapshot) {
    auto ctl = makeControls();
    RealtimeFacePipeline pipe(makeMutableHooks(ctl));
    std::future<RealtimeReloadResult> fut =
        pipe.reloadAsync(validManifest(), "", "arm64-v8a", "");
    const RealtimeReloadResult r = fut.get();
    EXPECT_TRUE(r.ok) << r.evidence;
    EXPECT_EQ(pipe.generation(), 1u);
    ASSERT_TRUE(pipe.snapshot());
    EXPECT_TRUE(pipe.snapshot()->effective);
}

// 析构安全：worker 尚未完成即销毁管线，析构须 join 等待，worker 不得 use-after-free；
// future 在管线销毁后仍能取到已就绪结果。
TEST(RealtimeFacePipeline, DestructorWaitsForPendingAsyncReload) {
    auto ctl = makeControls();
    ctl->selfCheckSleepUs = 50000;  // 50ms：确保析构发生在 worker 完成前
    std::future<RealtimeReloadResult> fut;
    {
        RealtimeFacePipeline pipe(makeMutableHooks(ctl));
        fut = pipe.reloadAsync(validManifest(), "", "arm64-v8a", "");
        // 立即离开作用域触发析构（worker 仍在自检中）。
    }
    const RealtimeReloadResult r = fut.get();  // 无 UAF，且 future 已就绪
    EXPECT_TRUE(r.ok) << r.evidence;
    EXPECT_EQ(r.generation, 1u);
}

// ── 构造函数写入机制：构造函数须把 .cpp 的 kDefaultEnforceBudget 写入 bench_ ──
//
// ⚠️ 本用例测的是**机制**——「构造函数是否真的把那行默认值写进了 bench_」（见
//    RealtimeFacePipeline.cpp:313 `bench_.enforceBudget = kDefaultEnforceBudget;`），
//    **不是**「平台默认值正确」。
//    本 target（core_unit_tests）被 CMake 注入 RK_BENCH_ENFORCE_BUDGET=1（cmake/core.cmake:131），
//    使 `#if defined(RK_BENCH_ENFORCE_BUDGET)` 在本 target **恒成立** ⇒ .cpp 的
//    `#elif defined(__aarch64__)` 与 `#else` 两个**平台回落分支在这里根本不被编译**。
//    ⇒ 「非 arm64 平台默认 = false」这条语义**不由本用例覆盖**：它由 core_gtest_tests 的
//      `RealtimeFacePipelinePlatformDefault.DefaultEnforceBudgetMatchesPlatform`
//      （tests/cpp/test_engine.cpp）承担——该 target 不定义此宏，故真走 `#else` 分支。
TEST(RealtimeFacePipeline, ConstructorWritesDefaultEnforceBudgetIntoBench) {
    // (a) 平台中立性回归护栏：拦住「把 #if __aarch64__ 放回头文件」的复发（arm64 上有牙；
    //     x64 上 #else 值与中立判定同值，故 x64 无判别力——见文末残余）。
    RealtimeBenchmarkConfig cfg;
    EXPECT_FALSE(cfg.enforceBudget)
        << "头文件默认值必须平台中立（恒 false）；架构相关默认只允许在 .cpp 的 "
           "kDefaultEnforceBudget 决定";

    // (b) 构造函数须把 kDefaultEnforceBudget 写入 bench_。期望值取自本 target 被 CMake 注入的
    //     RK_BENCH_ENFORCE_BUDGET（cmake/core.cmake:131）：它与 RealtimeFacePipeline.cpp 在同一 target
    //     内看到的宏逐字一致，且被刻意置为与头文件默认(false)相反 ⇒ 对本 target 的**构造写入**有判别力
    //     （删掉构造函数那行赋值，bench_ 退回头文件默认 false ⇒ 此处翻红）。
    //     下面这道守卫守的是「**本测试 target 的注入契约**」：注入一旦缺失（有人删掉 core.cmake 的
    //     target_compile_definitions、或误改判据），期望值推导会静默错位到未定义宏(=0)，故在编译期直接
    //     炸，而非让断言悄悄跑偏。
#if !defined(RK_BENCH_ENFORCE_BUDGET)
#error "本用例要求 core_unit_tests 注入 RK_BENCH_ENFORCE_BUDGET（见 cmake/core.cmake:131）"
#endif
    constexpr bool kExpectedDefaultEnforceBudget = (RK_BENCH_ENFORCE_BUDGET != 0);
    auto ctl = makeControls();
    RealtimeFacePipeline pipe(makeMutableHooks(ctl));
    EXPECT_EQ(pipe.benchmarkConfig().enforceBudget, kExpectedDefaultEnforceBudget)
        << "构造函数必须把 kDefaultEnforceBudget 写入 bench_；删掉该赋值此处即翻红";

    // (c) 默认管线在超预算时的行为须与（被注入的）默认值一致：默认强判 ⇒ 拒绝激活；默认只测量 ⇒ 照常激活。
    ctl->p95 = 500.0;  // 必然超预算（p95BudgetMs = 250.0）
    const RealtimeReloadResult r = pipe.reload(validManifest(), "", "arm64-v8a", "");
    if constexpr (kExpectedDefaultEnforceBudget) {
        EXPECT_FALSE(r.ok);
        EXPECT_EQ(r.reason, "runtime_init_failed");
    } else {
        EXPECT_TRUE(r.ok) << r.evidence;
    }
    // ★ 残余（如实）：本 target 因注入而**不覆盖** .cpp 的 #elif/#else 平台回落分支（见用例头注释）；
    //    该覆盖由 core_gtest_tests 的 RealtimeFacePipelinePlatformDefault 用例承担。头文件若重新引入
    //    __aarch64__ 分支，x64 上其 #else 值(false)与平台中立判定同值 ⇒ 该复发仅在 arm64 上可判别。
}
