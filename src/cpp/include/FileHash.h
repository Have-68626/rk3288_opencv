#pragma once

#include <string>
#include <filesystem>

namespace rk_wcfr {
    std::string calculateSHA256(const std::filesystem::path& filePath);
}
