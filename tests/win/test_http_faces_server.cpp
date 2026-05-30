#include "rk_win/HttpFacesServerPath.h"

#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>

bool test_http_faces_server_path_validation() {
    std::filesystem::path docRoot = std::filesystem::temp_directory_path() / "rk_http_faces_server_path_test_root";
    std::error_code ec;
    std::filesystem::remove_all(docRoot, ec);
    std::filesystem::create_directories(docRoot, ec);
    std::filesystem::create_directories(docRoot / "assets", ec);
    if (ec || !std::filesystem::exists(docRoot)) {
        std::cout << "FAIL: unable to create docRoot path=" << docRoot.string()
                  << " ec=" << ec.value() << " message=" << ec.message() << std::endl;
        return false;
    }
    {
        std::ofstream out(docRoot / "index.html");
        out.put('\n');
        out.close();
    }

    auto check = [&](const std::string& urlPath, bool expected) {
        std::filesystem::path out;
        bool ok = rk_win::isSafeRelativePath(docRoot, urlPath, out);
        if (ok != expected) {
            std::cout << "FAIL: urlPath=" << urlPath << " expected=" << expected << " actual=" << ok << std::endl;
            return false;
        }
        return true;
    };

    auto checkResolved = [&](const std::string& urlPath, const std::filesystem::path& expectedSuffix) {
        std::filesystem::path out;
        bool ok = rk_win::isSafeRelativePath(docRoot, urlPath, out);
        if (!ok) {
            std::cout << "FAIL: urlPath=" << urlPath << " expected resolved path but validation failed" << std::endl;
            return false;
        }
        const std::filesystem::path expectedPath = docRoot / expectedSuffix;
        if (out.lexically_normal() != expectedPath.lexically_normal()) {
            std::cout << "FAIL: urlPath=" << urlPath
                      << " expectedPath=" << expectedPath.string()
                      << " actualPath=" << out.string() << std::endl;
            return false;
        }
        return true;
    };

    bool allOk = true;

    // Safe paths
    allOk &= check("/index.html", true);
    allOk &= check("/assets/style.css", true);
    allOk &= check("/assets/logo%20new.png", true);
    allOk &= check("/assets/logo+new.png", true);
    allOk &= check("/assets/missing/deep/file.js", true);
    allOk &= check("/assets/a..b.png", true);
    allOk &= checkResolved("/assets/missing/deep/file.js", std::filesystem::path("assets") / "missing" / "deep" / "file.js");
    allOk &= checkResolved("/assets/a..b.png", std::filesystem::path("assets") / "a..b.png");

    // Malicious paths
    allOk &= check("../", false);
    allOk &= check("/../", false);
    allOk &= check("/assets/../../", false);
    allOk &= check("/..\\", false);
    allOk &= check("/%2e%2e/", false);
    allOk &= check("/assets/%2e%2e%2f", false);
    allOk &= check("/C:%5cWindows%5cSystem32", false);
    allOk &= check("/%5c%5cserver%5cshare", false);
    allOk &= check("//server/share", false);
    allOk &= check("/%zz%xx", false); // invalid hex

    // Incomplete hex encoding
    allOk &= check("/%", false);
    allOk &= check("/%A", false);
    allOk &= check("/abc%", false);

    // Test non-existent base directory
    std::filesystem::path fakeRoot = docRoot.parent_path() / "rk_http_faces_server_path_missing_root";
    std::filesystem::remove_all(fakeRoot, ec);
    std::filesystem::path out;
    bool ok = rk_win::isSafeRelativePath(fakeRoot, "/index.html", out);
    if (ok) {
        std::cout << "FAIL: Expected false for non-existent base directory." << std::endl;
        allOk = false;
    }

    // Symlink escape: skip if current environment cannot create the symlink.
    std::filesystem::path outsideRoot = docRoot.parent_path() / "rk_http_faces_server_path_outside_root";
    std::filesystem::remove_all(outsideRoot, ec);
    std::filesystem::create_directories(outsideRoot / "escape", ec);
    std::filesystem::path linkPath = docRoot / "link_outside";
    std::filesystem::remove(linkPath, ec);
    ec.clear();
    std::filesystem::create_directory_symlink(outsideRoot, linkPath, ec);
    if (ec) {
        std::cout << "TEST_SKIP name=http_faces_server_path_validation_symlink_escape reason=create_directory_symlink_failed code="
                  << ec.value() << " message=" << ec.message() << std::endl;
    } else {
        allOk &= check("/link_outside/escape/file.txt", false);
    }

    return allOk;
}
