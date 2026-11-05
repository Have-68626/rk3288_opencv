#pragma once

// 最小平台层：路径、环境、宽窄字符串、原子替换、终止信号。
// 目的：让 Windows 服务代码在进一步移植时不再直接依赖 Win32 API；本层不改变 Windows 行为。
// 约定：失败不抛异常，返回 false / 空值并写入可读错误；转换遇到非法序列按 U+FFFD 替换。

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace rk_win::platform {

// 可执行文件所在目录。Windows: GetModuleFileNameW；POSIX: /proc/self/exe。
// 解析失败时回落 std::filesystem::current_path()（与既有 Windows 回落一致）。
std::filesystem::path executableDir();

// 原子替换：src（已写好的临时文件）→ dst，并保证落盘。
// Windows: MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)。
// POSIX:   fsync(src) → rename(src, dst) → fsync(dst 的父目录)。
// 失败时 errOut 写入原因（含 errno 文本），返回 false。
bool replaceFileWriteThrough(const std::filesystem::path& src,
                             const std::filesystem::path& dst,
                             std::string& errOut);

// 把文件权限收窄为「仅属主可读写」（POSIX 0600）。Windows 为无操作（依赖用户目录继承 ACL）。
// 返回 false 表示无法确认权限已收窄，调用方应视为失败（密钥文件不得以宽权限留存）。
bool restrictFileOwnerOnly(const std::filesystem::path& path, std::string& errOut);

// 安装终止回调。Windows: SetConsoleCtrlHandler（Ctrl-C/Break/Close/Logoff/Shutdown）。
// POSIX: SIGINT / SIGTERM。回调在信号/控制事件上下文执行，只应设置原子标志。
// 同一进程只支持一个回调；重复安装会替换。
using TerminationCallback = void (*)();
bool installTerminationHandler(TerminationCallback callback);

// UTF-8 ↔ 宽字符。Windows 宽字符为 UTF-16；POSIX wchar_t 为 UTF-32。
// 非法序列替换为 U+FFFD（不抛异常、不截断）。
std::wstring wideFromUtf8(std::string_view utf8);
std::string utf8FromWide(std::wstring_view wide);

}  // namespace rk_win::platform
