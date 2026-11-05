#pragma once

// 套接字兼容层：Windows Winsock 与 POSIX socket 的统一最小面。
// 只覆盖服务代码实际用到的操作；停止 accept 由 shutdown 唤醒（POSIX 上 close 不保证唤醒）。
// POSIX 的 send 默认加 MSG_NOSIGNAL，避免对端断开触发 SIGPIPE 直接结束进程。

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rk_win::net {

#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
using SockLen = int;
#else
using Socket = int;
inline constexpr Socket kInvalidSocket = -1;
using SockLen = socklen_t;
#endif

// 进程级网络运行时初始化（Windows: WSAStartup；POSIX: 无操作）。
bool ensureRuntime();
void shutdownRuntime();

void closeSocket(Socket s);
void shutdownBoth(Socket s);

// 最近一次套接字错误码（Windows: WSAGetLastError；POSIX: errno）。
int lastError();

// 发送一段数据；返回实际发送字节数，<=0 表示失败/断开。
std::ptrdiff_t sendSome(Socket s, const void* data, std::size_t n);

}  // namespace rk_win::net
