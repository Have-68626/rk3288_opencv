#pragma once

#include "D3D11Renderer.h"
#include "Compat.h"

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

namespace rk_win {

struct RenderMetricsSample {
    std::string tsIso8601;
    RendererStats renderer;
    std::uint64_t rssBytes = 0;
    std::uint32_t handleCount = 0;
    Optional<std::uint64_t> gpuDedicatedBytes;
    Optional<std::uint64_t> gpuSharedBytes;
};

// 渲染指标日志：JSONL（一行一样本，append-only），落盘 `render_metrics.jsonl`。生产写入点是
// win_camera_face_recognition（旧版 UI target，默认 OFF）的 WM_TIMER(kTimerMetrics) 周期回调，
// 即**定时器节奏**写入（非逐帧），当前**无生产读取端**（消费方是运维/排查时的文本工具）。
// 轮转命名与 StructuredLogger / EventLogger 一致：`render_metrics.jsonl.1` … `render_metrics.jsonl.N`，
// **最新在 `.1`**，让三套日志共用同一套运维习惯（找「上一轮」= 看 `.1`）。
//
// ⚠️ 预算记账口径（本次改动**新登记**的事实，勿据旧文档以为它早已计入）：
//   · 本次改动**之前**，本文件是**唯一既无轮转、也不受任何尺寸上限约束**的日志 —— 旧 `open` 只收 dir，
//     `append` 只要流打开就逐条追加 ⇒ `render_metrics.jsonl` 可无限增长。
//   · 因此它在 `WinConfig.h` 的 512 MiB 日志总预算记账里**此前完全没有被计入**（该预算当时只按
//     recognition.csv / recognition.jsonl / event_log.jsonl **3 份**文件集均分，见 `kLogFileSetCount`）。
//   · 本次接线后（`open` 收满 maxBytes / maxRollFiles，由调用点传入 `cfg.log` 的同名键），本文件成为
//     **第 4 份**文件集 ⇒ `WinConfig.h` 的 `kLogFileSetCount` **已同步由 3 改为 4**（同一批次的另一
//     工作单元落地，已核实：该常量的定义与其派生量 `kLogPerSetBudgetBytes` / `kLogMaxFileBytesCap`
//     现为 134217728 / 67108864）。⚠️ 若将来有人把本类的接线退回「不收 maxBytes」，该份数须一并
//     退回 3，否则「512 MiB 总预算」的均分额会被高估（4 份各按 1/3 的均分额装载 ⇒ 最坏实占可达
//     4/3 × 512 MiB）。两者是**同一个记账口径的两端，必须同步增删**。
class RenderMetricsLogger {
public:
    // 语义与 EventLogger::open / StructuredLogger::open 完全一致：
    //   maxBytes == 0 或 maxRollFiles <= 0 ⇒ **关闭轮转**（文件无限增长）；到达上限即轮转。
    // 默认实参与 LogConfig（WinConfig.h）的默认值同值，且使「仅传 dir」的旧调用点源兼容。
    bool open(const std::filesystem::path& dir,
              std::uint64_t maxBytes = 10ULL * 1024ULL * 1024ULL,
              int maxRollFiles = 5);
    void close();

    void append(const RenderMetricsSample& s);

private:
    // 到达上限即轮转。**先 flush、再 close、再改名、再以 app 重开** —— 理由见 .cpp 内注释：Windows 上
    // 对仍被本进程打开的文件改名会以共享冲突（`ERROR_SHARING_VIOLATION`）失败，只 rename 不 close 的
    // 写法会让轮转静默失效；且改名后写句柄仍指向旧文件也会把后续样本写错档位。调用方须已持 mu_。
    void rollIfNeededLocked(const std::filesystem::path& path, std::ofstream& stream);

    std::mutex mu_;
    std::filesystem::path path_;
    std::uint64_t maxBytes_ = 0;
    int maxRollFiles_ = 0;
    std::ofstream out_;
};

std::uint64_t processRssBytes();
std::uint32_t processHandleCount();
Optional<std::pair<std::uint64_t, std::uint64_t>> gpuMemoryBytesBestEffort();

}  // namespace rk_win

