#include "NativeLog.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <time.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#define mkdir(dir, mode) _mkdir(dir)
#define access _access
#define F_OK 0
#define O_CREAT _O_CREAT
#define O_WRONLY _O_WRONLY
#define O_APPEND _O_APPEND
#define open _open
#define write _write
#define close _close
#define unlink _unlink
#define rename std::rename
#endif

namespace rklog {
namespace {
constexpr long kMaxBytes = 5L * 1024L * 1024L;
constexpr int kMaxRollFiles = 9;

std::mutex g_lock;
std::string g_internalDir;
std::string g_externalDir;
std::string g_fileName = "rk3288.log";

long getTid() {
#ifdef __linux__
    return static_cast<long>(syscall(SYS_gettid));
#elif defined(_WIN32)
    return static_cast<long>(GetCurrentThreadId());
#else
    return 0;
#endif
}

long long nowMs() {
#ifdef _WIN32
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
#else
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
#endif
}

bool ensureDir(const std::string& path) {
    if (path.empty()) return false;
    size_t pos = 0;
    while (true) {
        pos = path.find('/', pos + 1);
        std::string sub = (pos == std::string::npos) ? path : path.substr(0, pos);
        if (!sub.empty()) {
            mkdir(sub.c_str(), 0770);
        }
        if (pos == std::string::npos) break;
    }
    return true;
}

std::string joinPath(const std::string& dir, const char* name) {
    if (dir.empty()) return "";
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

off_t fileSize(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_size;
}

void rotateIfNeeded(const std::string& dir) {
    std::string current = joinPath(dir, g_fileName.c_str());
    if (fileSize(current) <= kMaxBytes) return;

    for (int i = kMaxRollFiles - 1; i >= 1; i--) {
        std::string older = current + "." + std::to_string(i);
        std::string newer = current + "." + std::to_string(i + 1);
        if (access(older.c_str(), F_OK) == 0) {
            unlink(newer.c_str());
            rename(older.c_str(), newer.c_str());
        }
    }

    std::string first = current + ".1";
    unlink(first.c_str());
    rename(current.c_str(), first.c_str());
}

void appendLineToDir(const std::string& dir, const std::string& line) {
    if (dir.empty()) return;
    ensureDir(dir);
    rotateIfNeeded(dir);

    std::string path = joinPath(dir, g_fileName.c_str());
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0660);
    if (fd < 0) return;
    write(fd, line.data(), line.size());
    write(fd, "\n", 1);
    close(fd);
}

std::string formatLine(const char* module, const char* function, const std::string& msg) {
    long tid = getTid();
    long long ms = nowMs();
    std::string line;
    line.reserve(msg.size() + 64);
    line.append("[");
    line.append(module ? module : "NATIVE");
    line.append("-");
    line.append(function ? function : "?");
    line.append("-");
    line.append(std::to_string(tid));
    line.append("-");
    line.append(std::to_string(ms));
    line.append("] ");
    line.append(msg);
    return line;
}

void logImpl(int prio, const char* module, const char* function, const std::string& msg) {
    std::string line = formatLine(module, function, msg);
#ifdef __ANDROID__
    __android_log_print(prio, module ? module : "NATIVE", "%s", line.c_str());
#else
    // Host fallback
    std::cout << line << std::endl;
#endif
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_internalDir.empty()) appendLineToDir(g_internalDir, line);
    if (!g_externalDir.empty()) appendLineToDir(g_externalDir, line);
}
}  // namespace

void setLogDirs(const std::string& internalDir, const std::string& externalDir, const std::string& filename) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_internalDir = internalDir;
    g_externalDir = externalDir;
    if (!filename.empty()) {
        g_fileName = filename;
    }
}

void logDebug(const char* module, const char* function, const std::string& msg) {
#ifdef __ANDROID__
    logImpl(ANDROID_LOG_DEBUG, module, function, msg);
#else
    logImpl(3, module, function, msg);
#endif
}

void logInfo(const char* module, const char* function, const std::string& msg) {
#ifdef __ANDROID__
    logImpl(ANDROID_LOG_INFO, module, function, msg);
#else
    logImpl(4, module, function, msg);
#endif
}

void logWarn(const char* module, const char* function, const std::string& msg) {
#ifdef __ANDROID__
    logImpl(ANDROID_LOG_WARN, module, function, msg);
#else
    logImpl(5, module, function, msg);
#endif
}

void logError(const char* module, const char* function, const std::string& msg) {
#ifdef __ANDROID__
    logImpl(ANDROID_LOG_ERROR, module, function, msg);
#else
    logImpl(6, module, function, msg);
#endif
}
}  // namespace rklog

