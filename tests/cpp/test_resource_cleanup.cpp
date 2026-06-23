#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

static std::string createTempFile() {
    auto tmp = std::filesystem::temp_directory_path();
    auto s = tmp.string() + "/rk_test_cleanup_" + std::to_string(std::rand());
    std::ofstream ofs(s);
    ofs << "test";
    ofs.close();
    return s;
}

}  // namespace

bool test_cleanup_temp_file_removed() {
    auto p = createTempFile();
    if (!std::filesystem::exists(p)) return false;
    std::filesystem::remove(p);
    if (std::filesystem::exists(p)) return false;
    return true;
}

bool test_cleanup_temp_dir_created_and_removed() {
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / ("rk_test_dir_" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    if (!std::filesystem::exists(dir)) return false;
    std::filesystem::remove_all(dir);
    if (std::filesystem::exists(dir)) return false;
    return true;
}

bool test_cleanup_file_write_and_read() {
    auto p = createTempFile();
    std::ifstream ifs(p);
    std::string content;
    ifs >> content;
    ifs.close();
    std::filesystem::remove(p);
    return content == "test";
}
