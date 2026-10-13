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
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <regex>
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
#define O_RDWR _O_RDWR
#define open _open
#define write _write
#define close _close
#define unlink _unlink
#define rename std::rename
#define fsync _commit
#endif

namespace rklog {
namespace {
constexpr long kMaxBytes = 5L * 1024L * 1024L;
constexpr int kMaxRollFiles = 9;

std::mutex g_lock;
std::string g_internalDir;
std::string g_externalDir;
std::string g_fileName = "rk3288.log";

// Persistent file descriptors (HR-58: avoid open/write/close per line)
int g_internalFd = -1;
int g_externalFd = -1;

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
        // Support both / and \ (HR-56)
        size_t n1 = path.find('/', pos + 1);
        size_t n2 = path.find('\\', pos + 1);
        if (n1 == std::string::npos && n2 == std::string::npos) break;
        if (n2 != std::string::npos && (n1 == std::string::npos || n2 < n1)) pos = n2;
        else pos = n1;
        std::string sub = path.substr(0, pos);
        if (!sub.empty()) {
            mkdir(sub.c_str(), 0770);
        }
    }
    return true;
}

std::string joinPath(const std::string& dir, const char* name) {
    if (dir.empty()) return "";
    char sep = dir.find('\\') != std::string::npos ? '\\' : '/';
    if (dir.back() == sep) return dir + name;
    return dir + sep + name;
}

off_t fileSize(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_size;
}

void rotateIfNeeded(const std::string& dir) {
    std::string current = joinPath(dir, g_fileName.c_str());
    if (fileSize(current) <= kMaxBytes) return;

    // Use temp file + rename for atomic rotation (HR-57)
    std::string tmp = current + ".rotating";
    for (int i = kMaxRollFiles - 1; i >= 1; i--) {
        std::string older = current + "." + std::to_string(i);
        std::string newer = current + "." + std::to_string(i + 1);
        if (access(older.c_str(), F_OK) == 0) {
            rename(older.c_str(), tmp.c_str());
            unlink(newer.c_str());
            rename(tmp.c_str(), newer.c_str());
        }
    }
    std::string first = current + ".1";
    unlink(first.c_str());
    rename(current.c_str(), first.c_str());
}

void ensureFileOpen(const std::string& dir, int& fd) {
    if (!dir.empty() && fd < 0) {
        ensureDir(dir);
        rotateIfNeeded(dir);
        std::string path = joinPath(dir, g_fileName.c_str());
        fd = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0660);
    }
}

void appendLineToDir(const std::string& dir, int& fd, const std::string& line) {
    if (dir.empty()) return;
    if (fd < 0) {
        ensureFileOpen(dir, fd);
    }
    if (fd < 0) return;
    // HR-55: check write return value
    if (write(fd, line.data(), line.size()) < 0) {
        close(fd);
        fd = -1;
        return;
    }
    if (write(fd, "\n", 1) < 0) {
        close(fd);
        fd = -1;
    }
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
    // 安全脱敏（S-SEC-B07-001）：logcat 与写盘两路共用本函数，在 formatLine 前
    // 统一掩码敏感 KV，一处脱敏即覆盖两路输出。
    const std::string redacted = maskSensitiveData(msg);
    std::string line = formatLine(module, function, redacted);
#ifdef __ANDROID__
    __android_log_print(prio, module ? module : "NATIVE", "%s", line.c_str());
#else
    std::cout << line << std::endl;
#endif
    // HR-54: build line outside lock, only hold lock briefly
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_internalDir.empty()) appendLineToDir(g_internalDir, g_internalFd, line);
    if (!g_externalDir.empty()) appendLineToDir(g_externalDir, g_externalFd, line);
}

// ── 通用日志脱敏辅助（S-SEC-B07-001）──────────────────────────────
// 敏感键快速预检清单（大小写不敏感）：仅当消息含任一敏感键子串时才进入正则掩码，
// 避免对绝大多数普通日志行造成正则开销（RK3288 Cortex-A17 性能约束，
// 与 Java 侧 AppLog.needsMasking 的“先子串预检再正则”思路一致）。
const char* const kSensitiveKeySubstrings[] = {
    "password", "passwd", "pwd", "token",
    "authorization", "bearer", "secret",
    "api_key", "api-key", "apikey",
};
constexpr size_t kSensitiveKeySubstringsCount =
    sizeof(kSensitiveKeySubstrings) / sizeof(kSensitiveKeySubstrings[0]);

// 大小写不敏感子串查找
bool containsIgnoreCase(const std::string& text, const char* needle) {
    const size_t needleLen = std::char_traits<char>::length(needle);
    if (needleLen == 0) return true;
    if (text.size() < needleLen) return false;
    const size_t max = text.size() - needleLen;
    for (size_t i = 0; i <= max; ++i) {
        size_t j = 0;
        for (; j < needleLen; ++j) {
            const unsigned char a = static_cast<unsigned char>(text[i + j]);
            const unsigned char b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) break;
        }
        if (j == needleLen) return true;
    }
    return false;
}

// 敏感 KV 值掩码：与 Java 侧 SensitiveDataUtil.maskSensitiveData 一致——
// 长度 >4 保留首 2 + "***" + 尾 2（兼顾可排查）；长度 ≤4 整体替换为 ***。
std::string maskKvValue(const std::string& value) {
    const size_t len = value.size();
    if (len <= 4) return "***";
    return value.substr(0, 2) + "***" + value.substr(len - 2);
}
}  // namespace

std::string maskSensitiveData(const std::string& msg) {
    // 快速预检：消息不含任何敏感键子串则原样返回（零分配、无正则开销）。
    bool hasCandidate = false;
    for (size_t i = 0; i < kSensitiveKeySubstringsCount; ++i) {
        if (containsIgnoreCase(msg, kSensitiveKeySubstrings[i])) {
            hasCandidate = true;
            break;
        }
    }
    if (!hasCandidate) return msg;

    // 通用 KV 掩码正则——与 Java 侧 SensitiveDataUtil.SENSITIVE_KV 对齐：
    //   键集合（password/passwd/pwd/token/access_token/api_key 等）+ 分隔符(=/:) + 值，
    //   值截至空白/逗号/分号/&（& 为 URL query 分隔，避免吞掉后续参数）。
    //   \b 词边界保证 "keyword="、"tokens" 等普通词不被误掩。
    //   SEC-04 补强（P3-4）：
    //     1) 键集补组合键 secret_key / client_secret —— 单键 \bsecret\b 遇下划线
    //        （同为词字符）尾边界失效，故组合键须以完整词形列出，值才能被捕获。
    //     2) 值前缀可选 "Bearer<空白>"：Authorization: Bearer <jwt> 场景下捕获组
    //        从 token 主体开始，掩掉整段 JWT，而 Bearer 前缀保留可见。
    //   静态局部正则编译一次、跨调用复用；C++11 保证线程安全初始化。
    static const std::regex kSensitiveKvPattern(
        R"(\b(password|passwd|pwd|token|access[_-]?token|refresh[_-]?token|)"
        R"(authorization|bearer|secret[_-]?key|client[_-]?secret|secret|api[_-]?key)\b)"
        R"([ \t\r\n]*[:=][ \t\r\n]*)"
        R"((?:Bearer[ \t\r\n]+)?([^ \t\r\n,;&]+))",
        std::regex_constants::icase);

    std::string out;
    out.reserve(msg.size());
    size_t last = 0;
    for (std::sregex_iterator it(msg.begin(), msg.end(), kSensitiveKvPattern), end;
         it != end; ++it) {
        const std::smatch& m = *it;
        // 键与分隔符原样保留（[m.position(0), m.position(2))），仅替换值。
        out.append(msg, last, m.position(0) - last);
        out.append(msg, m.position(0), m.position(2) - m.position(0));
        out.append(maskKvValue(m[2].str()));
        last = m.position(2) + m[2].length();
    }
    out.append(msg, last, msg.size() - last);
    return out;
}

void setLogDirs(const std::string& internalDir, const std::string& externalDir, const std::string& filename) {
    std::lock_guard<std::mutex> guard(g_lock);
    // Close old FDs before switching dirs (HR-58)
    if (g_internalFd >= 0) { close(g_internalFd); g_internalFd = -1; }
    if (g_externalFd >= 0) { close(g_externalFd); g_externalFd = -1; }
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
