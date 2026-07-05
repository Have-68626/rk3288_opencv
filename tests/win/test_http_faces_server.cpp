#include "rk_win/HttpFacesServerPath.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

TEST(HttpFacesServer, PathValidation) {
    std::filesystem::path docRoot = std::filesystem::temp_directory_path() / "rk_http_faces_server_path_test_root";
    std::error_code ec;
    std::filesystem::remove_all(docRoot, ec);
    std::filesystem::create_directories(docRoot, ec);
    std::filesystem::create_directories(docRoot / "assets", ec);
    ASSERT_TRUE(!ec && std::filesystem::exists(docRoot))
        << "unable to create docRoot path=" << docRoot.string()
        << " ec=" << ec.value() << " message=" << ec.message();
    {
        std::ofstream out(docRoot / "index.html");
        out.put('\n');
    }

    auto check = [&](const std::string& urlPath, bool expected) {
        std::filesystem::path out;
        bool ok = rk_win::isSafeRelativePath(docRoot, urlPath, out);
        EXPECT_EQ(ok, expected) << "urlPath=" << urlPath;
    };

    auto checkResolved = [&](const std::string& urlPath, const std::filesystem::path& expectedSuffix) {
        std::filesystem::path out;
        bool ok = rk_win::isSafeRelativePath(docRoot, urlPath, out);
        ASSERT_TRUE(ok) << "urlPath=" << urlPath;
        const std::filesystem::path expectedPath = docRoot / expectedSuffix;
        EXPECT_EQ(out.lexically_normal(), expectedPath.lexically_normal()) << "urlPath=" << urlPath;
    };

    // Safe paths
    check("/index.html", true);
    check("/assets/style.css", true);
    check("/assets/logo%20new.png", true);
    check("/assets/logo+new.png", true);
    check("/assets/missing/deep/file.js", true);
    check("/assets/a..b.png", true);
    checkResolved("/assets/missing/deep/file.js", std::filesystem::path("assets") / "missing" / "deep" / "file.js");
    checkResolved("/assets/a..b.png", std::filesystem::path("assets") / "a..b.png");

    // Malicious paths
    check("../", false);
    check("/../", false);
    check("/assets/../../", false);
    check("/..\\", false);
    check("/%2e%2e/", false);
    check("/assets/%2e%2e%2f", false);
    check("/C:%5cWindows%5cSystem32", false);
    check("/%5c%5cserver%5cshare", false);
    check("//server/share", false);
    check("/%zz%xx", false);

    // Incomplete hex encoding
    check("/%", false);
    check("/%A", false);
    check("/abc%", false);

    // Non-existent base directory
    std::filesystem::path fakeRoot = docRoot.parent_path() / "rk_http_faces_server_path_missing_root";
    std::filesystem::remove_all(fakeRoot, ec);
    std::filesystem::path out;
    bool ok = rk_win::isSafeRelativePath(fakeRoot, "/index.html", out);
    EXPECT_FALSE(ok) << "Expected false for non-existent base directory.";

    // Symlink escape: skip if current environment cannot create the symlink.
    std::filesystem::path outsideRoot = docRoot.parent_path() / "rk_http_faces_server_path_outside_root";
    std::filesystem::remove_all(outsideRoot, ec);
    std::filesystem::create_directories(outsideRoot / "escape", ec);
    std::filesystem::path linkPath = docRoot / "link_outside";
    std::filesystem::remove(linkPath, ec);
    ec.clear();
    std::filesystem::create_directory_symlink(outsideRoot, linkPath, ec);
    if (ec) {
        GTEST_SKIP() << "create_directory_symlink failed: " << ec.message();
    } else {
        check("/link_outside/escape/file.txt", false);
    }
}
