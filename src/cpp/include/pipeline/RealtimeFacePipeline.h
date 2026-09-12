#pragma once

#include "FaceRuntimeProfile.h"

#include <opencv2/core.hpp>  // cv::Mat（mock 亦可，便于 RK_SKIP_OPENCV 的 core_unit_tests 编译）

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// FaceSearchLinearIndex 定义于全局命名空间（FaceSearch.h）；此处仅前向声明，
// 快照以 shared_ptr<const> 持有图库状态，避免头文件依赖具体索引实现。
class FaceSearchLinearIndex;

namespace rk_core {

/**
 * 实时管线最小检测句柄契约。
 *
 * 有意不直接复用 rk_core::FaceDetector：后者经 FaceDetections 依赖 cv::Point2f，
 * 而 RK_SKIP_OPENCV 的 mock OpenCV 未提供该类，会导致 core_unit_tests 无法编译。
 * 该契约仅依赖 cv::Mat，使核心单元测试可注入假实现、生产侧由适配器桥接到
 * ScrfdNcnnAdapter（见 RealtimeFacePipeline.cpp 的默认装配钩子）。
 */
struct RealtimeFaceBox {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float score = 0.0f;
};

class IRealtimeDetector {
public:
    virtual ~IRealtimeDetector() = default;
    // 在 BGR 图上检测；成功返回 true 且 out 填检测框，失败置 err。
    virtual bool detect(const cv::Mat& bgr, std::vector<RealtimeFaceBox>& out,
                        std::string& err) = 0;
    virtual const char* name() const = 0;
};

/** 实时管线最小识别句柄契约（同上，不直接依赖 rk_core::Embedder）。 */
class IRealtimeEmbedder {
public:
    virtual ~IRealtimeEmbedder() = default;
    // 对已对齐人脸 BGR 提取嵌入；成功返回 true 且 out 填向量，失败置 err。
    virtual bool embed(const cv::Mat& alignedFaceBgr, std::vector<float>& out,
                       std::string& err) = 0;
    virtual int embeddingDim() const = 0;
    virtual const char* name() const = 0;
};

/**
 * 不可变运行时快照（任务11）。
 *
 * 一次 reload 成功后经 std::atomic_store 发布；推理侧 std::atomic_load 取快照并
 * 持有整个调用期间，reload 原子替换后旧快照引用归零才析构，故并发切换不产生 UAF。
 *
 * requested/effective/evidence/reason 对齐项目加速契约字段，reason 取值限定
 * ok/build_disabled/unsupported_platform/missing_dependency/missing_model/
 * runtime_init_failed/unsupported_input（见 AccelerationContract.h）。
 */
struct RealtimePipelineSnapshot {
    FaceRuntimeProfile profile;  // manifest v1 解析产物（不可变）
    std::shared_ptr<IRealtimeDetector> detector;
    std::shared_ptr<IRealtimeEmbedder> embedder;
    std::shared_ptr<const FaceSearchLinearIndex> gallery;  // 可为空（空图库）

    // ── 加速契约状态 ──
    bool requested = false;
    bool effective = false;
    std::string reason = "runtime_init_failed";
    std::string evidence;

    double benchmarkP95Ms = 0.0;
    std::uint64_t generation = 0;
};

/** reload 结果：失败时旧快照保持不变。 */
struct RealtimeReloadResult {
    bool ok = false;
    std::string reason;       // 契约 reason 码（成功为 "ok"）
    std::string evidence;     // 人类可读细节 / 失败原因
    std::uint64_t generation = 0;  // 成功为新代次；失败为当前（旧）代次
    double benchmarkP95Ms = 0.0;
};

/** 基准参数：高精度路径 10 预热 / 50 计时；ARM64 目标 p95 ≤ 250ms。 */
struct RealtimeBenchmarkConfig {
    int warmupIters = 10;
    int timedIters = 50;
    double p95BudgetMs = 250.0;
#if defined(__aarch64__)
    bool enforceBudget = true;  // ARM64（RK3288）强制预算
#else
    bool enforceBudget = false;  // 其他平台仅测量、不强判
#endif
};

/**
 * 装配钩子：把「加载 / 自检 / 基准」从协调逻辑中解耦，测试注入假实现，
 * 生产默认实现见 makeDefaultRealtimePipelineHooks()。
 *
 * 返回 nullptr 即视为该环节失败，须在 err 中写入契约 reason 码（或可被
 * normalizeReasonCode 归一的文本；未知文本归为 runtime_init_failed）。
 */
struct RealtimePipelineHooks {
    std::function<bool(const FaceRuntimeProfile&, const std::string& baseDir, std::string& err)>
        verifyFiles;
    std::function<std::shared_ptr<IRealtimeDetector>(const FaceRuntimeProfile&,
                                                     const std::string& baseDir,
                                                     std::string& err)>
        loadDetector;
    std::function<std::shared_ptr<IRealtimeEmbedder>(const FaceRuntimeProfile&,
                                                     const std::string& baseDir,
                                                     std::string& err)>
        loadEmbedder;
    // 图库加载可返回 nullptr + 空 err（视为空图库，合法）。
    std::function<std::shared_ptr<const FaceSearchLinearIndex>(const FaceRuntimeProfile&,
                                                               const std::string& galleryDir,
                                                               std::string& err)>
        loadGallery;
    std::function<bool(const RealtimePipelineSnapshot&, std::string& err)> selfCheck;
    // 基准：按 cfg 的预热/计时轮数测量，成功返回 true 并回填 p95 毫秒。
    std::function<bool(const RealtimePipelineSnapshot&, const RealtimeBenchmarkConfig& cfg,
                       double& p95Ms, std::string& err)>
        benchmark;
};

/** 生产默认钩子：manifest 文件校验（SHA/LFS）→ SCRFD/ArcFace 装配 → 真实推理自检/基准。 */
RealtimePipelineHooks makeDefaultRealtimePipelineHooks();

/**
 * 统一实时管线（任务11）：不可变快照 + 原子替换。
 *
 * 流程：后台解析/加载候选快照 → 自检推理 → 基准 → 一次 atomic_store 替换；
 * 任一环节失败保留旧快照并返回 reason。推理侧通过 snapshot()/detect()/embed()
 * 读取快照，切换期间持旧快照者不受影响。
 *
 * 线程安全：reload 之间由 reloadMu_ 串行；读取侧无锁（atomic_load）。
 * reload 是阻塞调用，设计为在后台线程执行；reloadAsync 提供便捷后台执行，
 * 其 worker 由本对象登记，析构时会等待未完成者结束（见 asyncWorkers_）。
 */
class RealtimeFacePipeline {
public:
    RealtimeFacePipeline();
    explicit RealtimeFacePipeline(RealtimePipelineHooks hooks);
    ~RealtimeFacePipeline();

    RealtimeFacePipeline(const RealtimeFacePipeline&) = delete;
    RealtimeFacePipeline& operator=(const RealtimeFacePipeline&) = delete;

    void setBenchmarkConfig(const RealtimeBenchmarkConfig& cfg);
    RealtimeBenchmarkConfig benchmarkConfig() const;

    /** 当前快照（原子读）。持有返回值期间即使发生 reload，旧快照也保持存活。 */
    std::shared_ptr<const RealtimePipelineSnapshot> snapshot() const;

    /** 当前代次：每次成功替换 +1（失败不变）。 */
    std::uint64_t generation() const;

    /** 同步重载：解析/校验/装配/自检/基准全部成功后一次替换；失败保留旧快照。 */
    RealtimeReloadResult reload(const std::string& manifestJson, const std::string& baseDir,
                                const std::string& currentAbi, const std::string& galleryDir);

    /**
     * 后台重载：工作线程执行 reload，立即返回 future（非阻塞）。
     * worker 由本对象登记；析构会 join 未完成者，故调用方无需再保证「future 就绪前
     * 本对象存活」——析构会安全等待，future 在 worker 结束后仍可取值。
     */
    std::future<RealtimeReloadResult> reloadAsync(std::string manifestJson, std::string baseDir,
                                                  std::string currentAbi, std::string galleryDir);

    // ── 读路径（使用当前快照，切换安全） ──
    bool detect(const cv::Mat& bgr, std::vector<RealtimeFaceBox>& out, std::string& err) const;
    bool embed(const cv::Mat& alignedFaceBgr, std::vector<float>& out, std::string& err) const;

private:
    // 由已校验 profile 装配候选快照并发布；调用方须已持有 reloadMu_。
    RealtimeReloadResult buildAndPublishLocked(const FaceRuntimeProfile& profile,
                                               const std::string& baseDir,
                                               const std::string& abi,
                                               const std::string& galleryDir,
                                               RealtimePipelineHooks& hooks,
                                               RealtimeBenchmarkConfig bench);

    RealtimePipelineHooks hooks_;
    mutable std::mutex cfgMu_;  // 保护 hooks_ 与 bench_（setBenchmarkConfig）
    RealtimeBenchmarkConfig bench_;

    std::mutex reloadMu_;  // 串行化 reload（buildAndPublish 期间）
    std::atomic<std::uint64_t> generation_{0};  // 成功替换次数，读取侧无锁

    // 原子替换的共享指针：所有读写必须经 std::atomic_load/atomic_store
    // （C++17 对 shared_ptr 的自由函数重载），不得裸读裸写。
    std::shared_ptr<const RealtimePipelineSnapshot> snapshot_;

    // ── reloadAsync worker 生命周期 ──
    // worker 捕获 this；若对象在 future 就绪前析构，worker 会 use-after-free。
    // 故 worker 句柄登记于此，析构时 join 全部未完成者；done 用于事后回收已结束
    // 的句柄，避免句柄表随 reloadAsync 调用次数无限增长。
    struct AsyncWorker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex asyncMu_;                  // 保护 asyncWorkers_
    std::vector<AsyncWorker> asyncWorkers_;
};

}  // namespace rk_core
