#include "NativeLog.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

namespace {

bool deleteFile(const std::string& path) {
#ifdef _WIN32
    return _unlink(path.c_str()) == 0;
#else
    return unlink(path.c_str()) == 0;
#endif
}

bool deleteDir(const std::string& path) {
#ifdef _WIN32
    return _rmdir(path.c_str()) == 0;
#else
    return rmdir(path.c_str()) == 0;
#endif
}

bool fileExists(const std::string& path) {
#ifdef _WIN32
    return _access(path.c_str(), 0) == 0;
#else
    return access(path.c_str(), F_OK) == 0;
#endif
}

std::string readAllText(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return content;
}

void cleanupTestDir(const std::string& dir, const std::string& logFileName) {
    std::string filePath = dir + "/" + logFileName;
    deleteFile(filePath);
    deleteDir(dir);
}

} // namespace

bool test_nativelog_creates_directory() {
    std::string testDir = "test_nativelog_dir_123";
    std::string logFile = "rk3288.log";
    cleanupTestDir(testDir, logFile);
    if (fileExists(testDir)) {
        std::cout << "test_nativelog_creates_directory: Pre-test cleanup failed." << std::endl;
        return false;
    }

    rklog::setLogDirs(testDir, "", logFile);
    rklog::logInfo("TEST_MOD", "test_func", "Hello Directory Creation");

    bool dirExists = fileExists(testDir);
    bool fileExistsFlag = fileExists(testDir + "/" + logFile);

    cleanupTestDir(testDir, logFile);
    // Reset to safe default to avoid side effects
    rklog::setLogDirs("", "", "rk3288.log");

    if (!dirExists) {
        std::cout << "test_nativelog_creates_directory: Directory was not created." << std::endl;
        return false;
    }
    if (!fileExistsFlag) {
        std::cout << "test_nativelog_creates_directory: File was not created." << std::endl;
        return false;
    }

    return true;
}

bool test_nativelog_writes_content() {
    std::string testDir = "test_nativelog_content_456";
    std::string logFile = "rk3288.log";
    cleanupTestDir(testDir, logFile);
    if (fileExists(testDir)) {
        std::cout << "test_nativelog_writes_content: Pre-test cleanup failed." << std::endl;
        return false;
    }

    rklog::setLogDirs(testDir, "", logFile);
    rklog::logInfo("TEST_MOD", "test_content", "Hello Content Write");

    std::string content = readAllText(testDir + "/" + logFile);
    bool contentFound = content.find("Hello Content Write") != std::string::npos;
    bool moduleFound = content.find("TEST_MOD") != std::string::npos;
    bool funcFound = content.find("test_content") != std::string::npos;

    cleanupTestDir(testDir, logFile);
    // Reset to safe default to avoid side effects
    rklog::setLogDirs("", "", "rk3288.log");

    if (!contentFound || !moduleFound || !funcFound) {
        std::cout << "test_nativelog_writes_content: Content mismatch. File content: " << content << std::endl;
        return false;
    }

    return true;
}
