#include <gtest/gtest.h>

#include "rk_win/Platform.h"
#include "rk_win/PlatformNet.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

TEST(PlatformTest, Utf8WideRoundTripPreservesAsciiCjkAndNonBmp) {
    const std::string samples[] = {
        "",
        "plain ascii",
        "中文与标点：，。",
        "emoji \xF0\x9F\x99\x82 and \xE2\x9C\x85",
    };
    for (const auto& s : samples) {
        EXPECT_EQ(rk_win::platform::utf8FromWide(rk_win::platform::wideFromUtf8(s)), s);
    }
}

TEST(PlatformTest, InvalidUtf8BecomesReplacementInsteadOfTruncation) {
    const std::string bad = "a\xFF" "b";
    const auto wide = rk_win::platform::wideFromUtf8(bad);
    const auto back = rk_win::platform::utf8FromWide(wide);
    EXPECT_NE(back, "ab");        // 旧实现逐字节截断会得到 "ab"
    EXPECT_NE(back.find("\xEF\xBF\xBD"), std::string::npos);  // U+FFFD
}

TEST(PlatformTest, ExecutableDirIsNonEmptyAndExists) {
    const auto dir = rk_win::platform::executableDir();
    EXPECT_FALSE(dir.empty());
    EXPECT_TRUE(fs::exists(dir));
}

TEST(PlatformTest, ReplaceFileWriteThroughReplacesContentAndRemovesSource) {
    const auto dir = fs::temp_directory_path() /
        ("rk_platform_replace_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const auto dst = dir / "target.txt";
    const auto src = dir / "target.txt.tmp";
    { std::ofstream(dst) << "old"; }
    { std::ofstream(src) << "new"; }
    std::string err;
    ASSERT_TRUE(rk_win::platform::replaceFileWriteThrough(src, dst, err)) << err;
    EXPECT_FALSE(fs::exists(src));
    std::ifstream in(dst);
    std::string content;
    std::getline(in, content);
    EXPECT_EQ(content, "new");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(PlatformTest, InstallTerminationHandlerSucceeds) {
    static int calls = 0;
    auto cb = []() { ++calls; };
    EXPECT_TRUE(rk_win::platform::installTerminationHandler(cb));
    EXPECT_EQ(calls, 0);  // 安装本身不触发
}

TEST(PlatformNetTest, RuntimeLifecycleAndInvalidSocketSendFail) {
    EXPECT_TRUE(rk_win::net::ensureRuntime());
    EXPECT_LE(rk_win::net::sendSome(rk_win::net::kInvalidSocket, "x", 1), 0);
    rk_win::net::shutdownRuntime();
}
