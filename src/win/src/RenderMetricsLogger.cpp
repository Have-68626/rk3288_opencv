#include "rk_win/RenderMetricsLogger.h"

#include "rk_win/LogRotation.h"
#include "rk_win/StructuredLogger.h"
#include "rk_win/WinStringUtil.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#include <filesystem>
#include <sstream>

namespace rk_win {
namespace {

// 与 StructuredLogger.cpp / EventLogger.cpp 中的同名辅助「各一份」：WinStringUtil.h 目前只把 escapeJson
// 统一了实现，fileSizeOr0 / renameBestEffort 尚未迁入共享头。把它们提到共享头需改 WinStringUtil.h，
// 不在本次改动的文件范围内 ⇒ 暂留本地副本（这是**已知并已登记**的重复，本次**不**做统一重构，避免把
// 「给本文件补上轮转」扩成「改公共头」）。
// Rollback: 若后续把这两个辅助统一到共享头，删本处副本、改为引用共享实现即可（调用点不变）。
std::uint64_t fileSizeOr0(const std::filesystem::path& p) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(sz);
}

// 先删目标再改名（Windows 上 rename 不覆盖已存在目标）；错误**有意吞掉**——轮转是尽力而为，
// 失败不得影响指标写入（此时日志继续追加到当前文件）。吞错的代价由 rollIfNeededLocked 的
// 「close → 轮转 → 重开」顺序兜住：即使改名没成功，写句柄也始终指向 path_ 本身。
// Rollback: 同 fileSizeOr0，统一到共享实现时删本处副本。
void renameBestEffort(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::remove(to, ec);
    ec.clear();
    std::filesystem::rename(from, to, ec);
}

}  // namespace

bool RenderMetricsLogger::open(const std::filesystem::path& dir, std::uint64_t maxBytes, int maxRollFiles) {
    std::lock_guard<std::mutex> lock(mu_);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    path_ = dir / "render_metrics.jsonl";
    maxBytes_ = maxBytes;
    maxRollFiles_ = maxRollFiles;
    // 幂等：重复 open() 必须先断开旧句柄，否则旧句柄会让后续轮转的改名吃共享冲突。
    if (out_.is_open()) out_.close();
    // 代数收缩清理（见 LogRotation.h）：maxRollFiles 由大调小时，索引大于新值的既有档位不会被
    // 任何一次轮转触及 ⇒ 永久滞留占盘。收缩只可能发生在两次 open 之间，故只在 open 做。
    // ⚠️ 本文件历史上**完全没有轮转** ⇒ 磁盘上可能存在任意大的代数（甚至从未轮转过）。
    //    清理只删 `> maxRollFiles` 的纯数字后缀档位，不会碰 `render_metrics.jsonl` 自身。
    pruneShrunkGenerations(path_, maxRollFiles_);
    // 冷启动即判一次：否则「上次退出时已超限」的文件要等首次 append 才轮转（判据滞后一轮）。
    rollIfNeededLocked(path_, out_);
    if (!out_.is_open()) out_.open(path_, std::ios::out | std::ios::app);
    return out_.is_open();
}

void RenderMetricsLogger::close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.is_open()) out_.close();
}

void RenderMetricsLogger::rollIfNeededLocked(const std::filesystem::path& path, std::ofstream& stream) {
    // 轮转关闭时（0 = 不限，语义与 EventLogger / StructuredLogger 一致）直接返回，不改变任何打开状态。
    if (maxBytes_ == 0 || maxRollFiles_ <= 0) return;

    // ★ 尺寸判据必须建立在**已落盘**的字节上：ofstream 有用户态缓冲，未 flush 的字节对
    //   std::filesystem::file_size 不可见 ⇒ 不先 flush 会让判据长期偏小、轮转被静默推迟甚至永不触发。
    //   `append` 现也逐条 flush（同 EventLogger），故此处的 flush 通常是幂等的重复；保留它是因为
    //   尺寸判据**不得依赖调用方的 flush 习惯**——本函数要自证「比尺寸前字节已落盘」。
    if (stream.is_open()) stream.flush();
    if (fileSizeOr0(path) <= maxBytes_) return;

    // ★★ 必须**先 close 自己的写句柄、再改名**：Windows 上 std::ofstream 打开的文件默认不带
    //    `FILE_SHARE_DELETE`，对仍被本进程打开的文件改名通常以 `ERROR_SHARING_VIOLATION` 失败；
    //    而 renameBestEffort 会把该错误吞掉 ⇒ 只 rename 不 close 会让轮转**静默失效**、文件继续无限
    //    增长（正是本类改动前的形态）。即便改名侥幸成功，写句柄仍指向已被改名的旧文件 ⇒ 之后的样本
    //    全部写进 `.1`，当前文件永远空着。故必须按「close → 轮转 → 以 app 重开」执行：任一环节失败
    //    都不会把日志写错文件。
    if (stream.is_open()) stream.close();

    // 代数迁移：`path.(N-1)` → `path.N`，最后 `path` → `path.1`（最新在 `.1`，与 StructuredLogger /
    // EventLogger 一致）。上限即 maxRollFiles_：`path.maxRollFiles_` 由 renameBestEffort 的「先删目标」
    // 覆盖掉，不会越界。
    for (int i = maxRollFiles_ - 1; i >= 1; --i) {
        const std::filesystem::path older = path.string() + "." + std::to_string(i);
        const std::filesystem::path newer = path.string() + "." + std::to_string(i + 1);
        if (std::filesystem::exists(older)) {
            renameBestEffort(older, newer);
        }
    }
    renameBestEffort(path, path.string() + ".1");

    // 轮转后立即重开：后续 append 继续写当前文件。重开失败（目录被删/无权限）时 stream 保持关闭，
    // append 会因 `!out_.is_open()` 而安全跳过，不写坏任何文件。
    stream.open(path, std::ios::out | std::ios::app);
}

void RenderMetricsLogger::append(const RenderMetricsSample& s) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!out_.is_open()) return;
    // 写前判一次轮转：本条 append 之后尺寸可能已超限，而轮转必须发生在**写入新样本之前**，否则
    // 当前文件会多留一条（用量上限就不再是 maxBytes + 一条）。轮转后重开可能失败 ⇒ 必须重新检查
    // is_open，不得往已关闭的流里写。
    rollIfNeededLocked(path_, out_);
    if (!out_.is_open()) return;  // 轮转后重开失败：本条样本丢弃，不得在错误状态下写

    /*
     * [Performance Optimization - string formatting]
     * Why: Avoid std::ostringstream overhead for render metrics logging in processing loops.
     * Impact: Reduced formatting overhead per log append.
     * Rollback: Revert to std::ostringstream concatenation.
     */
    std::string s_json;
    s_json.reserve(256);
    s_json += "{\"ts\":\"";
    s_json += escapeJson(s.tsIso8601);
    s_json += "\",\"rss_bytes\":";
    s_json += std::to_string(s.rssBytes);
    s_json += ",\"handle_count\":";
    s_json += std::to_string(s.handleCount);
    s_json += ",\"present_count\":";
    s_json += std::to_string(s.renderer.presentCount);
    s_json += ",\"present_fail_count\":";
    s_json += std::to_string(s.renderer.presentFailCount);
    s_json += ",\"device_removed_count\":";
    s_json += std::to_string(s.renderer.deviceRemovedCount);
    s_json += ",\"swapchain_recreate_count\":";
    s_json += std::to_string(s.renderer.swapchainRecreateCount);
    s_json += ",\"black_frame_count\":";
    s_json += std::to_string(s.renderer.blackFrameCount);

    char buf[256];
    std::snprintf(buf, sizeof(buf), ",\"frame_ms_last\":%g,\"frame_ms_p50\":%g,\"frame_ms_p95\":%g,\"frame_ms_p99\":%g",
                  s.renderer.frameTimes.lastMs, s.renderer.frameTimes.p50Ms,
                  s.renderer.frameTimes.p95Ms, s.renderer.frameTimes.p99Ms);
    s_json += buf;

    if (s.gpuDedicatedBytes && s.gpuSharedBytes) {
        s_json += ",\"gpu_dedicated_bytes\":";
        s_json += std::to_string(*s.gpuDedicatedBytes);
        s_json += ",\"gpu_shared_bytes\":";
        s_json += std::to_string(*s.gpuSharedBytes);
    }
    s_json += "}\n";
    out_ << s_json;
    // ★ 每次写入后 flush（口径同 EventLogger::append，本批新增）。
    //   为什么必需：① 轮转判据是**磁盘文件尺寸**，缓冲字节对 `file_size()` 不可见；② 运维用
    //   `tail -f` / `jq` 读这份指标文件时，不 flush 会让内容长时间不可见（本类是**定时器节奏**
    //   写入，缓冲攒满需要很久）；③ 进程崩溃时缓冲样本全丢。
    //   ⚠️ 这是**行为变更**：本类此前不 flush（其余两个日志器都 flush）。改动前该文件在运行期
    //   尺寸恒为滞后值，使「按尺寸轮转」在观测上不可验证。
    out_.flush();
}

std::uint64_t processRssBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return static_cast<std::uint64_t>(pmc.WorkingSetSize);
    return 0;
#else
    return 0;
#endif
}

std::uint32_t processHandleCount() {
#ifdef _WIN32
    DWORD c = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &c)) return static_cast<std::uint32_t>(c);
    return 0;
#else
    return 0;
#endif
}

Optional<std::pair<std::uint64_t, std::uint64_t>> gpuMemoryBytesBestEffort() {
#ifdef _WIN32
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullopt;
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(0, &adapter) != S_OK) return nullopt;
    ComPtr<IDXGIAdapter3> adapter3;
    if (FAILED(adapter.As(&adapter3))) return nullopt;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) return nullopt;
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal))) return nullopt;
    return std::make_pair(static_cast<std::uint64_t>(local.CurrentUsage), static_cast<std::uint64_t>(nonLocal.CurrentUsage));
#else
    return nullopt;
#endif
}

}  // namespace rk_win

