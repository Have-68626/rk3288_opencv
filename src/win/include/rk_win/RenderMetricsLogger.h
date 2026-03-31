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

class RenderMetricsLogger {
public:
    bool open(const std::filesystem::path& dir);
    void close();

    void append(const RenderMetricsSample& s);

private:
    static std::string escapeJson(const std::string& s);

    std::mutex mu_;
    std::filesystem::path path_;
    std::ofstream out_;
};

std::uint64_t processRssBytes();
std::uint32_t processHandleCount();
Optional<std::pair<std::uint64_t, std::uint64_t>> gpuMemoryBytesBestEffort();

}  // namespace rk_win

