#include "rk_win/HttpFacesServerPath.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#endif

namespace rk_win {

namespace {

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool isPathWithin(const std::filesystem::path& base, const std::filesystem::path& candidate) {
    auto baseIt = base.begin();
    auto candIt = candidate.begin();
    for (; baseIt != base.end() && candIt != candidate.end(); ++baseIt, ++candIt) {
        if (*baseIt != *candIt) return false;
    }
    return baseIt == base.end();
}

static bool isSafeRelativeSegments(const std::filesystem::path& relPath) {
    if (relPath.empty()) return false;
    if (relPath.has_root_name() || relPath.has_root_directory()) return false;
    for (const auto& segment : relPath) {
        const std::string value = segment.string();
        if (value.empty() || value == ".") continue;
        if (value == "..") return false;
    }
    return true;
}

static bool parseSafeUrlRelativePath(const std::string& urlPath, std::filesystem::path& relPathOut) {
    const std::string decoded = urlDecodePath(urlPath);
    if (decoded.empty() || decoded[0] != '/') return false;
    if (decoded.find(':') != std::string::npos ||
        decoded.find('\\') != std::string::npos ||
        decoded.find("//") != std::string::npos) {
        return false;
    }

    const std::filesystem::path rawRelPath(decoded.substr(1));
    if (!isSafeRelativeSegments(rawRelPath)) return false;

    relPathOut = rawRelPath.lexically_normal();
    return !relPathOut.empty();
}

#ifdef _WIN32
class WinHandle {
public:
    explicit WinHandle(HANDLE handle) : handle_(handle) {}
    ~WinHandle() { close(); }

    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle(WinHandle&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    HANDLE get() const { return handle_; }
    HANDLE release() { return std::exchange(handle_, INVALID_HANDLE_VALUE); }

private:
    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

    HANDLE handle_;
};

using NtCreateFileFn = NTSTATUS(NTAPI*)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
    ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

static HANDLE openRelativeObject(HANDLE parentHandle,
                                 const std::filesystem::path& namePath,
                                 ACCESS_MASK desiredAccess,
                                 ULONG createOptions,
                                 ULONG shareAccess) {
    std::wstring name = namePath.wstring();
    std::replace(name.begin(), name.end(), L'/', L'\\');
    if (name.empty() || name.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t)) {
        return INVALID_HANDLE_VALUE;
    }

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto ntCreateFile = ntdll == nullptr
        ? nullptr
        : reinterpret_cast<NtCreateFileFn>(GetProcAddress(ntdll, "NtCreateFile"));
    if (ntCreateFile == nullptr) return INVALID_HANDLE_VALUE;

    UNICODE_STRING objectName{};
    objectName.Buffer = name.data();
    objectName.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    objectName.MaximumLength = objectName.Length;

    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &objectName, OBJ_CASE_INSENSITIVE, parentHandle, nullptr);
    IO_STATUS_BLOCK ioStatus{};
    HANDLE fileHandle = INVALID_HANDLE_VALUE;
    const NTSTATUS status = ntCreateFile(
        &fileHandle, desiredAccess, &attributes, &ioStatus,
        nullptr, FILE_ATTRIBUTE_NORMAL, shareAccess, FILE_OPEN,
        createOptions | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT, nullptr, 0);
    return NT_SUCCESS(status) ? fileHandle : INVALID_HANDLE_VALUE;
}

static bool isReparsePoint(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    return !GetFileInformationByHandle(handle, &info) ||
           (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// 每一层均相对已经固定的父目录句柄打开。FILE_OPEN_REPARSE_POINT 使链接本身
// 而非其目标被打开，因此任何 symlink/junction 都在读取前 fail-closed。
static HANDLE openNoReparsePointRelativeToRootImpl(HANDLE rootHandle,
                                                   const std::filesystem::path& relativePath,
                                                   bool leafMustBeDirectory) {
    HANDLE parentHandle = rootHandle;
    WinHandle currentDirectory(INVALID_HANDLE_VALUE);
    for (auto it = relativePath.begin(); it != relativePath.end(); ++it) {
        const bool isLeaf = std::next(it) == relativePath.end();
        const bool openDirectory = !isLeaf || leafMustBeDirectory;
        WinHandle opened(openRelativeObject(
            parentHandle, *it,
            openDirectory ? FILE_READ_ATTRIBUTES | SYNCHRONIZE
                          : FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            openDirectory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE,
            openDirectory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ));
        if (opened.get() == INVALID_HANDLE_VALUE || isReparsePoint(opened.get())) {
            return INVALID_HANDLE_VALUE;
        }
        if (isLeaf) return opened.release();

        currentDirectory = std::move(opened);
        parentHandle = currentDirectory.get();
    }
    return INVALID_HANDLE_VALUE;
}

static HANDLE openNoReparsePointRelativeToRoot(HANDLE rootHandle,
                                               const std::filesystem::path& relativePath) {
    return openNoReparsePointRelativeToRootImpl(rootHandle, relativePath, false);
}

static HANDLE openNoReparseDirectoryRelativeToRoot(HANDLE rootHandle,
                                                    const std::filesystem::path& relativePath) {
    return openNoReparsePointRelativeToRootImpl(rootHandle, relativePath, true);
}

// docRoot 的每个祖先也按固定句柄逐层打开，避免服务启动窗口跟随被替换的
// symlink/junction。卷根是此次路径解析唯一的字符串起点。
static HANDLE openNoReparseDirectoryTree(const std::filesystem::path& docRoot) {
    std::error_code ec;
    const std::filesystem::path absoluteRoot = std::filesystem::absolute(docRoot, ec).lexically_normal();
    if (ec || !absoluteRoot.has_root_name() || !absoluteRoot.has_root_directory()) {
        return INVALID_HANDLE_VALUE;
    }

    const std::filesystem::path volumeRoot = absoluteRoot.root_path();
    const std::filesystem::path relativeRoot = absoluteRoot.relative_path();
    if (volumeRoot.empty() || !isSafeRelativeSegments(relativeRoot)) {
        return INVALID_HANDLE_VALUE;
    }

    WinHandle volumeHandle(CreateFileW(
        volumeRoot.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (volumeHandle.get() == INVALID_HANDLE_VALUE || isReparsePoint(volumeHandle.get())) {
        return INVALID_HANDLE_VALUE;
    }
    return openNoReparseDirectoryRelativeToRoot(volumeHandle.get(), relativeRoot);
}
#endif

static bool resolveWithExistingAncestor(
    const std::filesystem::path& baseCanon,
    const std::filesystem::path& targetLex,
    std::filesystem::path& resolvedOut) {
    std::error_code ec;
    std::filesystem::path existing = targetLex;
    std::vector<std::filesystem::path> missingSuffix;

    while (!std::filesystem::exists(existing, ec)) {
        if (ec) return false;
        const std::filesystem::path parent = existing.parent_path();
        if (parent.empty() || parent == existing) return false;
        missingSuffix.push_back(existing.filename());
        existing = parent;
    }

    std::filesystem::path existingCanon = std::filesystem::weakly_canonical(existing, ec);
    if (ec) return false;
    if (!isPathWithin(baseCanon, existingCanon)) return false;

    std::filesystem::path rebuilt = existingCanon;
    for (auto it = missingSuffix.rbegin(); it != missingSuffix.rend(); ++it) {
        rebuilt /= *it;
    }
    resolvedOut = rebuilt.lexically_normal();
    return true;
}

}  // namespace

std::string urlDecodePath(const std::string& url) {
    std::string out;
    out.reserve(url.size());
    for (std::size_t i = 0; i < url.size(); ++i) {
        if (url[i] == '%') {
            if (i + 2 < url.size()) {
                int h1 = hexValue(url[i + 1]);
                int h2 = hexValue(url[i + 2]);
                if (h1 >= 0 && h2 >= 0) {
                    out += static_cast<char>((h1 << 4) | h2);
                    i += 2;
                } else {
                    // Invalid hex encoding, fail validation.
                    return "";
                }
            } else {
                // Incomplete percent-encoding sequence at the end of the string.
                return "";
            }
        } else {
            out += url[i];
        }
    }
    return out;
}

bool isSafeRelativePath(const std::filesystem::path& docRoot, const std::string& urlPath, std::filesystem::path& resolvedOut) {
    std::error_code ec;
    // Base dir must exist and be accessible
    if (!std::filesystem::exists(docRoot, ec) || ec) return false;

    std::filesystem::path baseCanon = std::filesystem::weakly_canonical(docRoot, ec);
    if (ec) return false;

    std::filesystem::path relPath;
    if (!parseSafeUrlRelativePath(urlPath, relPath)) return false;

    std::filesystem::path targetLex = (baseCanon / relPath).lexically_normal();
    return resolveWithExistingAncestor(baseCanon, targetLex, resolvedOut);
}

StaticFileRoot::~StaticFileRoot() {
    close();
}

bool StaticFileRoot::open(const std::filesystem::path& docRoot) {
    close();
#ifdef _WIN32
    HANDLE handle = openNoReparseDirectoryTree(docRoot);
    if (handle == INVALID_HANDLE_VALUE) return false;

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            FILE_ATTRIBUTE_DIRECTORY) {
        CloseHandle(handle);
        return false;
    }

    nativeHandle_ = reinterpret_cast<std::uintptr_t>(handle);
    return true;
#else
    fallbackRoot_ = docRoot;
    return true;
#endif
}

void StaticFileRoot::close() {
#ifdef _WIN32
    if (nativeHandle_ != 0) {
        CloseHandle(reinterpret_cast<HANDLE>(nativeHandle_));
        nativeHandle_ = 0;
    }
#endif
    fallbackRoot_.clear();
}

bool StaticFileRoot::resolve(const std::string& urlPath, std::filesystem::path& resolvedOut) const {
    std::filesystem::path relativePath;
    if (!parseSafeUrlRelativePath(urlPath, relativePath)) return false;

#ifdef _WIN32
    if (nativeHandle_ == 0) return false;
#else
    if (fallbackRoot_.empty()) return false;
#endif
    resolvedOut = std::move(relativePath);
    return true;
}

bool readFileWithinRoot(const StaticFileRoot& docRoot,
                        const std::filesystem::path& path,
                        std::string& out) {
    out.clear();
#ifdef _WIN32
    if (docRoot.nativeHandle_ == 0) return false;
    if (!isSafeRelativeSegments(path)) return false;

    const HANDLE rootHandle = reinterpret_cast<HANDLE>(docRoot.nativeHandle_);
    // RootDirectory 让相对路径直接绑定固定根对象；逐层拒绝重解析点，避免再依赖
    // 可被祖先重绑定竞态欺骗的最终路径字符串比较。
    WinHandle fileHandle(openNoReparsePointRelativeToRoot(rootHandle, path));
    if (fileHandle.get() == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(fileHandle.get(), &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > (std::numeric_limits<std::size_t>::max)()) {
        return false;
    }

    out.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < out.size()) {
        const auto remaining = out.size() - offset;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            remaining, (std::numeric_limits<DWORD>::max)()));
        DWORD read = 0;
        if (!ReadFile(fileHandle.get(), out.data() + offset, request, &read, nullptr) || read != request) {
            out.clear();
            return false;
        }
        offset += read;
    }
    return true;
#else
    (void)docRoot;
    (void)path;
    return false;
#endif
}

} // namespace rk_win
