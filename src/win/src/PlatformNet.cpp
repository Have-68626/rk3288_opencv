#include "rk_win/PlatformNet.h"

#include <cerrno>

namespace rk_win::net {

bool ensureRuntime() {
#ifdef _WIN32
    WSADATA wsa{};
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void shutdownRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
}

void closeSocket(Socket s) {
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

void shutdownBoth(Socket s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    ::shutdown(s, SD_BOTH);
#else
    // 先 shutdown 再 close：POSIX 上 close 不保证唤醒同进程里阻塞的 accept/recv。
    ::shutdown(s, SHUT_RDWR);
#endif
}

int lastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::ptrdiff_t sendSome(Socket s, const void* data, std::size_t n) {
    if (n == 0) return 0;
#ifdef _WIN32
    return ::send(s, static_cast<const char*>(data), static_cast<int>(n), 0);
#else
    const ssize_t w = ::send(s, data, n, MSG_NOSIGNAL);
    if (w < 0 && errno == EINTR) return 0;  // 调用方据此判定断开；EINTR 与断开同处理
    return static_cast<std::ptrdiff_t>(w);
#endif
}

}  // namespace rk_win::net
