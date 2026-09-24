#include "pipeline/RealtimeFacePipeline.h"

#include "FaceSearch.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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
    // 非空 → loadGallery 返回 nullptr 并把该值写入 err（触发「图库加载失败」分支）。
    // 注意：「返回 nullptr + 空 err」是**合法空图库**，不触发失败分支。
    std::string galleryErr;
    // verifyFiles 返回 false 时写入 err 的文本（仅 verifyOk=false 时使用）。
    // 默认取 FaceRuntimeManifest::verifyFiles 的真实文案之一——**不要**写 "model_missing" 这类
    // 既非固定码、也不是 verifyFiles 文案的字符串：它经 normalizeReasonCode 会落成
    // runtime_init_failed，容易让断言写成与实现无关的错值。
    std::string verifyErr = "缺失模型文件: model.param";
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
            err = ctl->verifyErr;
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
    h.loadGallery = [ctl](const FaceRuntimeProfile&, const std::string&,
                          std::string& err) -> std::shared_ptr<const FaceSearchLinearIndex> {
        if (!ctl->galleryErr.empty()) {
            err = ctl->galleryErr;  // nullptr + 非空 err ⇒ 走「图库加载失败」分支
            return nullptr;
        }
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

// ── 供 FaceRuntimeManifest::verifyFiles 的**真实实现**用例使用的两个辅助 ──────────────

// 临时基目录（仿 tests/cpp/test_face_runtime_profile.cpp 的 tempBase() 范式；名字独立，避免与其
// 它用例的临时目录互相干扰）。调用方**必须**自行清理，见用例内的 remove_all。
std::filesystem::path verifyCouplingTempBase() {
    return std::filesystem::temp_directory_path() / "rk3288_opencv_pipeline_verify_coupling";
}

// 与 validManifest() 同构，但给 detector.param 钉一个 sha256（用于触发「摘要不匹配」分支）。
// 值取 64 个 'a'：只要文件内容不是「SHA-256 恰等于 64 个 a」的怪文件，就必然不等——本用例只要求
// 「格式合法 + 不相等」，不必真算摘要。锚点未命中时返回**空串**（不返回无 sha 的 manifest）：
// 否则调用方会拿到一个语义不同的 manifest 而断言仍可能通过，属静默失真。
std::string manifestWithDetectorParamSha256() {
    std::string m = validManifest();
    const std::string anchor = "\"param\":{\"path\":\"scrfd/scrfd.param\"}";
    const std::size_t pos = m.find(anchor);
    if (pos == std::string::npos) return std::string();
    m.insert(pos + anchor.size() - 1,  // 插在该对象闭合 '}' 之前
             ",\"sha256\":\"" + std::string(64, 'a') + "\"");
    return m;
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
//    RealtimeFacePipeline.cpp 构造函数内的 `bench_.enforceBudget = kDefaultEnforceBudget;`），
//    **不是**「平台默认值正确」。
//    本 target（core_unit_tests）被 CMake 注入 RK_BENCH_ENFORCE_BUDGET=1（cmake/core.cmake 中
//    core_unit_tests 的 RK_BENCH_ENFORCE_BUDGET 注入），
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
    //     RK_BENCH_ENFORCE_BUDGET（见 cmake/core.cmake 中 core_unit_tests 的注入语句）：它与
    //     RealtimeFacePipeline.cpp 在同一 target 内看到的宏逐字一致，且被刻意置为与头文件默认(false)
    //     相反 ⇒ 对本 target 的**构造写入**有判别力（删掉构造函数那行赋值，bench_ 退回头文件默认
    //     false ⇒ 此处翻红）。
    //     下面这道守卫守的是「**本测试 target 的注入契约**」：注入一旦缺失（有人删掉 core.cmake 的
    //     target_compile_definitions、或误改判据），期望值推导会静默错位到未定义宏(=0)，故在编译期直接
    //     炸，而非让断言悄悄跑偏。
#if !defined(RK_BENCH_ENFORCE_BUDGET)
#error "本用例要求 core_unit_tests 注入 RK_BENCH_ENFORCE_BUDGET（见 cmake/core.cmake 的 core_unit_tests 注入）"
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

// ── FACE-PIPE-010 ②：图库加载失败的 reason 必须由 err 归一得出 ──────────────────

// 图库加载失败（钩子返回 nullptr **且** err 非空）时，reason 由 reasonFromLoadError(err) 归一，
// 而非硬编码 missing_model：否则自定义钩子报出的「非缺文件」类失败会被误导性地写成 missing_model。
// 判别点：把该分支改回硬编码 "missing_model" ⇒ 本条翻红。
// ⚠️ 判别力的前提是**钩子写的码与 missing_model 不同**（本例用 unsupported_input）：若写成
//    err = "missing_model"，改前改后同值 ⇒ 无判别力。
TEST(RealtimeFacePipeline, GalleryLoadFailureReasonComesFromErr) {
    auto ctl = makeControls();
    ctl->galleryErr = "unsupported_input";
    RealtimeFacePipeline pipe(makeMutableHooks(ctl));

    const RealtimeReloadResult r = pipe.reload(validManifest(), "", "arm64-v8a", "");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, "unsupported_input");
    EXPECT_EQ(r.generation, 0u);       // 首次 reload 即失败：无成功代次可言
    EXPECT_EQ(pipe.generation(), 0u);
    EXPECT_EQ(pipe.snapshot(), nullptr);
    // err 原文随 evidence 透传（此行改前改后一致，**不是**本次修复带来的差异，只作回归护栏）
    EXPECT_NE(r.evidence.find("图库加载失败: unsupported_input"), std::string::npos);
}

// FACE-PIPE-010 ②/新3：verifyFiles 失败时 reason **按 err 语义分类**（reasonFromVerifyError），
// 不再一律 missing_model。判据不是「等于某个我猜的码」，而是**两类语义不同的失败必须落到不同的码上**。
// 判别点（两条，任一即红）：
//   · 改回硬编码 missing_model            ⇒ 两次得到的 reason 相同
//   · 改回无脑套 reasonFromLoadError      ⇒ 两条人类文本都归一成 runtime_init_failed，仍相同
// 另附兜底分支：钩子直接写固定码时原样透传（不是人类文本 ⇒ 交回 reasonFromLoadError）。
//
// ⚠️ **本用例的判别力边界（如实登记，勿高估）**：下面两条 err 文本**抄自**生产文案
//    FaceRuntimeManifest::verifyFiles（src/cpp/src/FaceRuntimeProfile.cpp 的 "缺失模型文件: " 与
//    "SHA-256 不匹配: "），但它们是写进**可变钩子**（Controls::verifyErr）的字面串——本用例
//    **从不调用**真实 verifyFiles ⇒ 它钉住的只是 RealtimeFacePipeline.cpp 的 `reasonFromVerifyError`
//    关键字映射（子串语义）这**一侧**。改 FaceRuntimeProfile.cpp 的 err 文案时，本用例**不会**翻红
//    （映射会**静默**退化：未知文本 → runtime_init_failed）。提醒：不要把这些字面串当随手测试数据删改，
//    它们是「关键字映射仍与文案约定一致」的快照；而「生产文案本身仍在约定关键字上」这一侧由下面
//    紧邻的 VerifyFilesProductionTextStaysCoupledToReasonClassification 用例用**真实** verifyFiles 钉住。
//    （若将来改为「verifyFiles 出参直接给 reasonCode」，两个用例都应改为按新契约断言，而不是继续复刻字面串。）
TEST(RealtimeFacePipeline, VerifyFilesFailureReasonFollowsErrSemantics) {
    auto ctl = makeControls();
    RealtimeFacePipeline pipe(makeMutableHooks(ctl));
    ctl->verifyOk = false;

    // 语义一：文件不在位 → missing_model（唯一应报此码的语义）
    ctl->verifyErr = "缺失模型文件: det.param";
    const RealtimeReloadResult missing = pipe.reload(validManifest(), "", "arm64-v8a", "");
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.reason, "missing_model");
    EXPECT_EQ(missing.generation, 0u);
    EXPECT_EQ(pipe.snapshot(), nullptr);
    EXPECT_NE(missing.evidence.find("缺失模型文件: det.param"), std::string::npos)
            << "err 原文一直随 evidence 透传（改前改后一致）";

    // 语义二：内容不符（摘要不匹配）→ 另一个码（unsupported_input）
    ctl->verifyErr = "SHA-256 不匹配: det.param";
    const RealtimeReloadResult corrupt = pipe.reload(validManifest(), "", "arm64-v8a", "");
    EXPECT_FALSE(corrupt.ok);
    EXPECT_EQ(corrupt.reason, "unsupported_input");
    EXPECT_NE(corrupt.evidence.find("SHA-256 不匹配: det.param"), std::string::npos);

    // 核心断言：语义不同 ⇒ 码必须不同
    EXPECT_NE(missing.reason, corrupt.reason)
            << "两类语义不同的校验失败给出同一个 reason 码，说明分类未生效";

    // 兜底：钩子直接写契约固定码时原样透传（既非人类文本，走 reasonFromLoadError）
    ctl->verifyErr = "build_disabled";
    EXPECT_EQ(pipe.reload(validManifest(), "", "arm64-v8a", "").reason, "build_disabled");
}

// FACE-PIPE-010 ②（补齐侧）：**生产文案本身**与 reason 分类的**端到端**耦合。
//
// 为什么需要（A2 审计实证的缺口）：上面那条用例把 err 文本写进可变钩子，只钉住 `reasonFromVerifyError`
// 的关键字映射这一侧；改 FaceRuntimeProfile.cpp 里 verifyFiles 的文案（如把 "缺失模型文件: " 改字）
// 会让生产 reason 从 missing_model **静默**退化为 runtime_init_failed，而**全仓无红**。本用例把
// `verifyFiles` 钩子直接设为**生产实现** `FaceRuntimeManifest::verifyFiles`，让真实文案经真实归类路径
// 产出 reason，从而同时钉住两侧。
//
// 判别点（改坏任一处即红，且失败信息能指出是哪一侧）：
//   · 改 FaceRuntimeProfile.cpp 的 "缺失模型文件: " / "SHA-256 不匹配: " 文案 ⇒ evidence 断言与 reason
//     断言同时翻红（reason 退化为 runtime_init_failed）；
//   · 改 RealtimeFacePipeline.cpp 的 reasonFromVerifyError 关键字 ⇒ reason 断言翻红（evidence 仍绿）；
//   · 把该分支改回硬编码 missing_model ⇒ 摘要不匹配那一档翻红（两类语义不再落不同码）。
//
// ⚠️ 判据依赖文件系统（真实 verifyFiles 读盘）：用 temp_directory_path() 下的**独立**子目录，用例
//    开始/结束各清理一次；「文件不在位」一档刻意不创建任何文件。若临时目录不可写，本用例会**失败**
//    （不是静默通过）——写盘在本仓库既有 core_unit_tests 用例（test_face_runtime_profile.cpp、
//    test_file_hash.cpp 等）中已是既定范式。
// ⚠️ 未覆盖（如实登记）：verifyFiles 的另两类文案（"非法模型路径…" / "Git LFS pointer（未拉取）"）
//    未在此端到端用例中出现——前者被 parseV1 先行拒绝（插不进 manifest），后者需再构造一个内容为
//    LFS pointer 的文件。它们目前仍只由「字面串钩子」的上一条用例（文案→码的映射一侧）覆盖。
TEST(RealtimeFacePipeline, VerifyFilesProductionTextStaysCoupledToReasonClassification) {
    const std::filesystem::path base = verifyCouplingTempBase();
    std::error_code ec;
    std::filesystem::remove_all(base, ec);  // 清掉上次运行/崩溃的残留，保证「文件不在位」这一前提成立
    ASSERT_FALSE(std::filesystem::exists(base)) << "临时基目录无法清除，本用例前提不成立";

    RealtimePipelineHooks h;
    // ★ 关键：钩子**就是生产实现**（不是字面串复制品）。其余钩子**刻意留空**：verifyFiles 若意外通过
    //（例如模型文件真的在位），后续会走「detector 装配失败」分支 ⇒ reason 变成别的码、断言失败，
    // 而**不会**静默通过。
    h.verifyFiles = &rk_core::FaceRuntimeManifest::verifyFiles;
    RealtimeFacePipeline pipe(h);

    // (a) 语义一：模型文件不在位 ⇒ 生产文案「缺失模型文件: <相对路径>」⇒ missing_model
    const RealtimeReloadResult missing =
        pipe.reload(validManifest(), base.string(), "arm64-v8a", "");
    ASSERT_FALSE(missing.ok);
    EXPECT_NE(missing.evidence.find("模型文件校验失败: 缺失模型文件: scrfd/scrfd.param"),
              std::string::npos)
            << "生产 verifyFiles 的「缺失模型文件」文案变了（或条目次序变了）："
               "src/cpp/src/FaceRuntimeProfile.cpp";
    EXPECT_EQ(missing.reason, "missing_model")
            << "production reason 未由生产文案归类得出（文案与 reasonFromVerifyError 关键字不再对齐）";
    EXPECT_EQ(missing.generation, 0u);
    EXPECT_EQ(pipe.snapshot(), nullptr);

    // (b) 语义二：文件在位但摘要不符 ⇒ 生产文案「SHA-256 不匹配: <相对路径>」⇒ unsupported_input
    //     只需创建 detector.param 一个文件：verifyFiles 的条目次序里 detectorParam 最先，命中即返回。
    std::filesystem::create_directories(base / "scrfd", ec);
    {
        std::ofstream out(base / "scrfd" / "scrfd.param", std::ios::binary);
        out << "X";  // 内容任意：只要其 SHA-256 不等于清单里钉的 64 个 'a'
    }
    const std::string manifest = manifestWithDetectorParamSha256();
    ASSERT_FALSE(manifest.empty()) << "锚点未命中：manifestWithDetectorParamSha256 的插入点已漂移";
    const RealtimeReloadResult corrupt = pipe.reload(manifest, base.string(), "arm64-v8a", "");
    ASSERT_FALSE(corrupt.ok);
    EXPECT_NE(corrupt.evidence.find("SHA-256 不匹配: scrfd/scrfd.param"), std::string::npos)
            << "生产 verifyFiles 的「SHA-256 不匹配」文案变了：src/cpp/src/FaceRuntimeProfile.cpp";
    EXPECT_EQ(corrupt.reason, "unsupported_input")
            << "两类语义不同的校验失败必须落到不同的码；落到同一个码说明分类已静默退化";

    std::filesystem::remove_all(base, ec);
}
