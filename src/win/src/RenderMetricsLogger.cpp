#include "rk_win/RenderMetricsLogger.h"

#include "rk_win/StructuredLogger.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#include <filesystem>
#include <sstream>

namespace rk_win {

std::string RenderMetricsLogger::escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

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

    std::ostringstream oss;
    oss << "{"
        << "\"ts\":\"" << escapeJson(s.tsIso8601) << "\","
        << "\"rss_bytes\":" << s.rssBytes << ","
        << "\"handle_count\":" << s.handleCount << ","
        << "\"present_count\":" << s.renderer.presentCount << ","
        << "\"present_fail_count\":" << s.renderer.presentFailCount << ","
        << "\"device_removed_count\":" << s.renderer.deviceRemovedCount << ","
        << "\"swapchain_recreate_count\":" << s.renderer.swapchainRecreateCount << ","
        << "\"black_frame_count\":" << s.renderer.blackFrameCount << ","
        << "\"frame_ms_last\":" << s.renderer.frameTimes.lastMs << ","
        << "\"frame_ms_p50\":" << s.renderer.frameTimes.p50Ms << ","
        << "\"frame_ms_p95\":" << s.renderer.frameTimes.p95Ms << ","
        << "\"frame_ms_p99\":" << s.renderer.frameTimes.p99Ms;

    if (s.gpuDedicatedBytes && s.gpuSharedBytes) {
        oss << ",\"gpu_dedicated_bytes\":" << *s.gpuDedicatedBytes << ",\"gpu_shared_bytes\":" << *s.gpuSharedBytes;
    }
    oss << "}\n";
    out_ << oss.str();
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

