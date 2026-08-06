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

// 覆盖 SHA-256 填充边界：55/56（单/双填充块）、63/64/65（完整尾块前后）、
// 65535/65536/65537/65601（64KB 读缓冲边界）。期望值由可信实现
// （Python hashlib）对确定性序列 bytes(i & 0xFF for i in range(size)) 计算。
TEST(FileHash, BoundaryLengths) {
    const struct { std::size_t size; const char* digest; } cases[] = {
        {0,     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {55,    "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {56,    "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {63,    "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {64,    "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {65,    "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
        {127,   "92ca0fa6651ee2f97b884b7246a562fa71250fedefe5ebf270d31c546bfea976"},
        {65535, "5f1bf999bcba5e05d4c34a13710d2e4bff005877874dcce49ac87af61076231e"},
        {65536, "7daca2095d0438260fa849183dfc67faa459fdf4936e1bc91eec6b281b27e4c2"},
        {65537, "2deb0bd2129a9d3aed91e3cff58b3993752be549642890a3e853ec1065f9b617"},
        {65601, "5f43edae005679c3c7378d522d828b602283de823509a2dd0371c0891fd009f2"},
    };
    for (const auto& c : cases) {
        std::filesystem::path tempPath = std::filesystem::temp_directory_path() /
            ("test_hash_boundary_" + std::to_string(c.size) + ".bin");
        TempFileCleaner cleaner(tempPath);

        {
            std::ofstream out(tempPath, std::ios::binary);
            ASSERT_TRUE(out) << "failed to create temp file for size " << c.size;
            for (std::size_t i = 0; i < c.size; ++i) {
                out.put(static_cast<char>(i & 0xFF));
            }
        }

        std::string actual = rk_wcfr::calculateSHA256(tempPath);
        EXPECT_EQ(actual, c.digest) << "size=" << c.size;
    }
}
