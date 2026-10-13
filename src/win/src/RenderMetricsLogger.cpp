#include "rk_win/RenderMetricsLogger.h"

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

bool RenderMetricsLogger::open(const std::filesystem::path& dir) {
    std::lock_guard<std::mutex> lock(mu_);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    path_ = dir / "render_metrics.jsonl";
    out_.open(path_, std::ios::out | std::ios::app);
    return out_.is_open();
}

void RenderMetricsLogger::close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.is_open()) out_.close();
}

void RenderMetricsLogger::append(const RenderMetricsSample& s) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!out_.is_open()) return;

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

