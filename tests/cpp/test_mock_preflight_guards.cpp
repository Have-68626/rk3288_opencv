#include "VideoManager.h"

using namespace rk_core;

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path findFixture(const std::string& name) {
    const auto cwd = std::filesystem::current_path();
    const std::vector<std::filesystem::path> candidates = {
        cwd / "tests" / "fixtures" / "mock" / name,
        cwd.parent_path() / "tests" / "fixtures" / "mock" / name
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && !ec) {
            return p;
        }
    }
    return {};
}

std::filesystem::path makeTempDir(const std::string& suffix) {
    const auto base = std::filesystem::temp_directory_path() / "rk3288_opencv_mock_preflight_tests";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = base / (suffix + "_" + std::to_string(stamp));
    std::filesystem::create_directories(dir, ec);
    return dir;
}

}  // namespace

bool test_mock_preflight_rejects_invalid_magic() {
    const auto path = findFixture("corrupt_magic.jpg");
    if (path.empty()) return false;

    std::string reason;
    const bool ok = VideoManager::preflightMockInput(path.string(), &reason);
    if (ok) return false;
    if (reason != "MOCK_MAGIC_INVALID") return false;
    return true;
}

bool test_mock_preflight_rejects_incomplete_file() {
    const auto path = findFixture("incomplete.jpg");
    if (path.empty()) return false;

    std::string reason;
    const bool ok = VideoManager::preflightMockInput(path.string(), &reason);
    if (ok) return false;
    if (reason != "MOCK_FILE_INCOMPLETE") return false;
    return true;
}

bool test_mock_preflight_rejects_oversize_image() {
    const auto dir = makeTempDir("oversize");
    const auto path = dir / "oversize.jpg";
    {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) return false;
        out.seekp(static_cast<std::streamoff>(50LL * 1024LL * 1024LL));
        out.put('\0');
    }

    std::string reason;
    const bool ok = VideoManager::preflightMockInput(path.string(), &reason);
    if (ok) return false;
    if (reason != "MOCK_OVERSIZE") return false;
    return true;
}
