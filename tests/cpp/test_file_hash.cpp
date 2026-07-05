#include "FileHash.h"
#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <string>

namespace {

// RAII wrapper to ensure temporary files are deleted.
class TempFileCleaner {
public:
    explicit TempFileCleaner(const std::filesystem::path& path) : path_(path) {}
    ~TempFileCleaner() {
        if (std::filesystem::exists(path_)) {
            std::filesystem::remove(path_);
        }
    }
private:
    std::filesystem::path path_;
};

} // namespace

TEST(FileHash, KnownContent) {
    std::filesystem::path tempPath = std::filesystem::temp_directory_path() / "test_hash_hello.txt";
    TempFileCleaner cleaner(tempPath);

    {
        std::ofstream out(tempPath, std::ios::binary);
        ASSERT_TRUE(out) << "failed to create temp file";
        out << "hello world";
    }

    // SHA256 of "hello world"
    std::string expected = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";
    std::string actual = rk_wcfr::calculateSHA256(tempPath);

    EXPECT_EQ(actual, expected);
}

TEST(FileHash, EmptyFile) {
    std::filesystem::path tempPath = std::filesystem::temp_directory_path() / "test_hash_empty.txt";
    TempFileCleaner cleaner(tempPath);

    {
        std::ofstream out(tempPath, std::ios::binary);
        ASSERT_TRUE(out) << "failed to create temp file";
    }

    // SHA256 of empty string
    std::string expected = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::string actual = rk_wcfr::calculateSHA256(tempPath);

    EXPECT_EQ(actual, expected);
}

TEST(FileHash, InvalidPath) {
    std::filesystem::path invalidPath = std::filesystem::temp_directory_path() / "test_hash_does_not_exist_12345.txt";

    // Ensure it doesn't exist
    if (std::filesystem::exists(invalidPath)) {
        std::filesystem::remove(invalidPath);
    }

    std::string actual = rk_wcfr::calculateSHA256(invalidPath);

    EXPECT_EQ(actual, "");
}
