#include "rk_win/DisplaySettings.h"

#ifdef _WIN32
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#include <algorithm>

namespace rk_win {

double DisplayMode::refreshHz() const {
    if (refreshDenominator == 0) return 0.0;
    return static_cast<double>(refreshNumerator) / static_cast<double>(refreshDenominator);
}

bool DisplaySettings::refresh() {
    outputs_.clear();

#ifdef _WIN32
    using Microsoft::WRL::ComPtr;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    for (UINT adapterIndex = 0;; adapterIndex++) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        for (UINT outputIndex = 0;; outputIndex++) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;

            UINT modeCount = 0;
            if (FAILED(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &modeCount, nullptr))) continue;
            if (modeCount == 0) continue;

            std::vector<DXGI_MODE_DESC> dxgiModes(modeCount);
            if (FAILED(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &modeCount, dxgiModes.data()))) continue;

            DisplayOutput out;
            out.name = desc.DeviceName;
            out.monitor = desc.Monitor;
            out.desktopRect = desc.DesktopCoordinates;
            out.modes.reserve(dxgiModes.size());

            for (const auto& m : dxgiModes) {
                DisplayMode dm;
                dm.width = static_cast<int>(m.Width);
                dm.height = static_cast<int>(m.Height);
                dm.refreshNumerator = m.RefreshRate.Numerator;
                dm.refreshDenominator = m.RefreshRate.Denominator ? m.RefreshRate.Denominator : 1;
                dm.interlaced = (m.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST) ||
                                (m.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST);
                out.modes.push_back(dm);
            }

            auto key = [](const DisplayMode& a) {
                const std::uint64_t w = static_cast<std::uint64_t>(a.width) & 0xFFFF;
                const std::uint64_t h = static_cast<std::uint64_t>(a.height) & 0xFFFF;
                const std::uint64_t rn = static_cast<std::uint64_t>(a.refreshNumerator) & 0xFFFFFF;
                const std::uint64_t rd = static_cast<std::uint64_t>(a.refreshDenominator) & 0xFF;
                return (w << 48) | (h << 32) | (rn << 8) | rd;
            };

            std::sort(out.modes.begin(), out.modes.end(), [&](const DisplayMode& a, const DisplayMode& b) { return key(a) < key(b); });
            out.modes.erase(std::unique(out.modes.begin(), out.modes.end(), [&](const DisplayMode& a, const DisplayMode& b) { return key(a) == key(b); }),
                            out.modes.end());

            outputs_.push_back(std::move(out));
        }
    }

    return !outputs_.empty();
#else
    return false;
#endif
}

const std::vector<DisplayOutput>& DisplaySettings::outputs() const {
    return outputs_;
}

DisplayModeValidationResult DisplaySettings::validateMode(int outputIndex, const DisplayMode& mode) const {
    DisplayModeValidationResult r;
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) {
        r.ok = false;
        r.reason = "输出索引无效";
        return r;
    }
    if (mode.width <= 0 || mode.height <= 0) {
        r.ok = false;
        r.reason = "分辨率无效";
        return r;
    }
    if (mode.refreshDenominator == 0 || mode.refreshNumerator == 0) {
        r.ok = false;
        r.reason = "刷新率无效";
        return r;
    }

    const auto& modes = outputs_[static_cast<size_t>(outputIndex)].modes;
    const bool exists = std::any_of(modes.begin(), modes.end(), [&](const DisplayMode& m) {
        return m.width == mode.width && m.height == mode.height && m.refreshNumerator == mode.refreshNumerator && m.refreshDenominator == mode.refreshDenominator;
    });
    if (!exists) {
        r.ok = false;
        r.reason = "系统不支持该分辨率/刷新率组合";
        return r;
    }

    r.ok = true;
    return r;
}

std::optional<DisplayMode> DisplaySettings::findMode(int outputIndex, int width, int height, std::uint32_t refreshN, std::uint32_t refreshD) const {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) return std::nullopt;
    const auto& modes = outputs_[static_cast<size_t>(outputIndex)].modes;
    for (const auto& m : modes) {
        if (m.width == width && m.height == height && m.refreshNumerator == refreshN && m.refreshDenominator == (refreshD ? refreshD : 1)) {
            return m;
        }
    }
    return std::nullopt;
}

}  // namespace rk_win

