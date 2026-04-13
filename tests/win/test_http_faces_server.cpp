#include "rk_win/HttpFacesServerPath.h"

#include <filesystem>
#include <iostream>
#include <string>

bool test_http_faces_server_path_validation() {
    std::filesystem::path docRoot = std::filesystem::current_path() / "test_root";
    std::error_code ec;
    std::filesystem::create_directories(docRoot, ec);

    auto check = [&](const std::string& urlPath, bool expected) {
        std::filesystem::path out;
        bool ok = rk_win::isSafeRelativePath(docRoot, urlPath, out);
        if (ok != expected) {
            std::cout << "FAIL: urlPath=" << urlPath << " expected=" << expected << " actual=" << ok << std::endl;
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

    // Test non-existent base directory
    std::filesystem::path fakeRoot = std::filesystem::current_path() / "does_not_exist";
    std::filesystem::path out;
    bool ok = rk_win::isSafeRelativePath(fakeRoot, "/index.html", out);
    if (ok) {
        std::cout << "FAIL: Expected false for non-existent base directory." << std::endl;
        allOk = false;
    }

    return allOk;
}
