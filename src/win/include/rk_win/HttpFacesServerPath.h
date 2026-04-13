#pragma once

#include <filesystem>
#include <string>

namespace rk_win {

std::string urlDecodePath(const std::string& url);
bool isSafeRelativePath(const std::filesystem::path& docRoot, const std::string& urlPath, std::filesystem::path& resolvedOut);

} // namespace rk_win
