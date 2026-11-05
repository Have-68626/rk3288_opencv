#include "rk_win/Platform.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rk_win::platform {
namespace {

#ifdef _WIN32
TerminationCallback g_terminationCallback = nullptr;

BOOL WINAPI consoleCtrlHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_terminationCallback) g_terminationCallback();
        return TRUE;
    default:
        return FALSE;
    }
}
#else
TerminationCallback g_terminationCallback = nullptr;

extern "C" void posixTerminationHandler(int) {
    if (g_terminationCallback) g_terminationCallback();
}

#if defined(__linux__) && !defined(__ANDROID__)
std::filesystem::path executableDirFromProc() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::filesystem::path(buf).parent_path();
}
#endif

void appendUtf8(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// 解一个 UTF-8 码点。非法时返回 U+FFFD 并只前进一个字节（与 Windows 的多字节转换容忍度一致）。
char32_t decodeUtf8(std::string_view s, std::size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](std::size_t k) -> bool {
        return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    };
    if (b0 < 0x80) { ++i; return b0; }
    if ((b0 & 0xE0) == 0xC0 && cont(1)) {
        const char32_t cp = ((b0 & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
        i += 2;
        return cp >= 0x80 ? cp : 0xFFFD;
    }
    if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        const char32_t cp = ((b0 & 0x0F) << 12) |
                            ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                            (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        i += 3;
        return cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF) ? cp : 0xFFFD;
    }
    if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        const char32_t cp = ((b0 & 0x07) << 18) |
                            ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                            ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                            (static_cast<unsigned char>(s[i + 3]) & 0x3F);
        i += 4;
        return cp >= 0x10000 && cp <= 0x10FFFF ? cp : 0xFFFD;
    }
    ++i;
    return 0xFFFD;
}
#endif  // _WIN32

}  // namespace

std::filesystem::path executableDir() {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::filesystem::current_path();
    return std::filesystem::path(path).parent_path();
#else
#if defined(__linux__) && !defined(__ANDROID__)
    const auto dir = executableDirFromProc();
    if (!dir.empty()) return dir;
#endif
    return std::filesystem::current_path();
#endif
}

bool replaceFileWriteThrough(const std::filesystem::path& src,
                             const std::filesystem::path& dst,
                             std::string& errOut) {
    errOut.clear();
#ifdef _WIN32
    if (MoveFileExW(src.wstring().c_str(), dst.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        errOut = "MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH) 失败: " +
                 std::to_string(static_cast<unsigned long>(GetLastError()));
        return false;
    }
    return true;
#else
    // 先 fsync 临时文件，保证 rename 之前数据已落盘；再 fsync 父目录，让目录项持久化。
    const int fd = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        errOut = std::string("open(src) 失败: ") + std::strerror(errno);
        return false;
    }
    const int syncRc = ::fsync(fd);
    const int syncErr = errno;
    ::close(fd);
    if (syncRc != 0) {
        errOut = std::string("fsync(src) 失败: ") + std::strerror(syncErr);
        return false;
    }
    if (::rename(src.c_str(), dst.c_str()) != 0) {
        errOut = std::string("rename 失败: ") + std::strerror(errno);
        return false;
    }
    const auto parent = dst.parent_path();
    const std::string dir = parent.empty() ? std::string(".") : parent.string();
    const int dirFd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
    }
    return true;
#endif
}

bool restrictFileOwnerOnly(const std::filesystem::path& path, std::string& errOut) {
    errOut.clear();
#ifdef _WIN32
    (void)path;  // NTFS 权限由用户目录继承；不另设 ACL。
    return true;
#else
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        errOut = std::string("chmod(0600) 失败: ") + std::strerror(errno);
        return false;
    }
    return true;
#endif
}

bool installTerminationHandler(TerminationCallback callback) {
#ifdef _WIN32
    g_terminationCallback = callback;
    return SetConsoleCtrlHandler(consoleCtrlHandler, TRUE) != FALSE;
#else
    g_terminationCallback = callback;
    struct sigaction action {};
    action.sa_handler = posixTerminationHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return ::sigaction(SIGINT, &action, nullptr) == 0 &&
           ::sigaction(SIGTERM, &action, nullptr) == 0;
#endif
}

std::wstring wideFromUtf8(std::string_view utf8) {
    if (utf8.empty()) return {};
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
#else
    static_assert(sizeof(wchar_t) == 4, "POSIX 宽字符按 UTF-32 处理；其他宽度需另写实现");
    std::wstring out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        out.push_back(static_cast<wchar_t>(decodeUtf8(utf8, i)));
    }
    return out;
#endif
}

std::string utf8FromWide(std::wstring_view wide) {
    if (wide.empty()) return {};
#ifdef _WIN32
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
#else
    static_assert(sizeof(wchar_t) == 4, "POSIX 宽字符按 UTF-32 处理；其他宽度需另写实现");
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t wc : wide) {
        const char32_t cp = static_cast<char32_t>(wc);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            appendUtf8(out, 0xFFFD);
        } else {
            appendUtf8(out, cp);
        }
    }
    return out;
#endif
}

}  // namespace rk_win::platform
