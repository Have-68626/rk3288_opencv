#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rk_win {

struct DisplayMode {
    int width = 0;
    int height = 0;
    std::uint32_t refreshNumerator = 0;
    std::uint32_t refreshDenominator = 1;
    bool interlaced = false;

    double refreshHz() const;
};

struct DisplayOutput {
    std::wstring name;
#ifdef _WIN32
    HMONITOR monitor = nullptr;
    RECT desktopRect{};
#endif
    std::vector<DisplayMode> modes;
};

struct DisplayModeValidationResult {
    bool ok = false;
    std::string reason;
};

class DisplaySettings {
public:
    bool refresh();
    const std::vector<DisplayOutput>& outputs() const;

    DisplayModeValidationResult validateMode(int outputIndex, const DisplayMode& mode) const;
    std::optional<DisplayMode> findMode(int outputIndex, int width, int height, std::uint32_t refreshN, std::uint32_t refreshD) const;

private:
    std::vector<DisplayOutput> outputs_;
};

}  // namespace rk_win

