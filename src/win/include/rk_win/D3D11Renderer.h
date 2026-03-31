#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace cv {
class Mat;
}

namespace rk_win {

enum class PreviewScaleMode {
    CropFill = 0,
    Letterbox = 1,
};

struct RenderOptions {
    bool vsync = true;
    int bufferCount = 2;
    bool fullscreen = false;
    bool allowSystemModeSwitch = false;

    int modeWidth = 0;
    int modeHeight = 0;
    std::uint32_t refreshNumerator = 0;
    std::uint32_t refreshDenominator = 1;

    bool enableSRGB = true;
    float gamma = 2.2f;
    int colorTempK = 6500;

    int aaSamples = 1;
    int anisoLevel = 1;
};

struct FrameTimeStats {
    double lastMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
};

struct RendererStats {
    std::uint64_t presentCount = 0;
    std::uint64_t presentFailCount = 0;
    std::uint64_t deviceRemovedCount = 0;
    std::uint64_t swapchainRecreateCount = 0;
    std::uint64_t blackFrameCount = 0;
    FrameTimeStats frameTimes;
};

class D3D11Renderer {
public:
    D3D11Renderer();
    ~D3D11Renderer();

    bool initialize(HWND hwnd);
    void shutdown();

    bool reconfigure(const RenderOptions& opt);
    bool renderFrame(const cv::Mat* bgr);
    void onResize(int clientW, int clientH);

    void setPreviewScaleMode(PreviewScaleMode mode);
    PreviewScaleMode previewScaleMode() const;

    RendererStats stats() const;
    RenderOptions options() const;
    std::vector<int> supportedMsaaSampleCounts() const;
    int maxAnisotropy() const;

    std::string lastError() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace rk_win

