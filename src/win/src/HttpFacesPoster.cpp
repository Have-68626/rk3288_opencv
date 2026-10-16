#include "rk_win/HttpFacesPoster.h"

#include "rk_win/EventLogger.h"
#include "rk_win/FacesJson.h"
#include "rk_win/FramePipeline.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

#include <algorithm>
#include <chrono>

namespace rk_win {
namespace {

#ifdef _WIN32
std::wstring wFromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool isValidDestination(const std::string& value) {
    // INI、环境变量不经过 JSON PUT 校验，发送入口必须独立收口。
    if (value.empty() || std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c <= 0x20 || c == 0x7f;
    })) return false;
    const std::wstring url = wFromUtf8(value);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    if (url.empty() || !WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.dwHostNameLength == 0 ||
        parts.nPort == 0 || (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) return false;
    // WinHTTP 只拆分 URL，方括号内的内容仍须验证为 IPv6 字面量。
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    if (host.find_first_of(L"[]") != std::wstring::npos) {
        if (host.size() < 3 || host.front() != L'[' || host.back() != L']') return false;
        IN6_ADDR address{};
        return InetPtonW(AF_INET6, host.substr(1, host.size() - 2).c_str(), &address) == 1;
    }
    return true;
}

bool httpPostJson(const std::string& urlUtf8, const std::string& body, int timeoutMs, int& statusOut) {
    statusOut = 0;
    std::wstring url = wFromUtf8(urlUtf8);
    if (url.empty()) return false;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256];
    wchar_t path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET sess = WinHttpOpen(L"rk_wcfr/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return false;
    WinHttpSetTimeouts(sess, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    std::wstring hostW(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring pathW(uc.lpszUrlPath, uc.dwUrlPathLength);
    HINTERNET conn = WinHttpConnect(sess, hostW.c_str(), uc.nPort, 0);
    if (!conn) {
        WinHttpCloseHandle(sess);
        return false;
    }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", pathW.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(sess);
        return false;
    }

    const wchar_t* hdr = L"Content-Type: application/json; charset=utf-8\r\n";
    BOOL ok = WinHttpSendRequest(req, hdr, -1, const_cast<LPVOID>(static_cast<const void*>(body.data())), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok) WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    statusOut = static_cast<int>(status);

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);

    return ok && (status >= 200 && status < 300);
}
#endif

}  // namespace

HttpFacesPoster::HttpFacesPoster() = default;

HttpFacesPoster::~HttpFacesPoster() {
    stop();
}

bool HttpFacesPoster::start(FramePipeline* pipe, EventLogger* events, const std::string& postUrl, int throttleMs, int backoffMinMs, int backoffMaxMs) {
    stop();
    if (!pipe) return false;
#ifdef _WIN32
    if (!isValidDestination(postUrl)) return false;
#else
    return false;
#endif
    pipe_ = pipe;
    events_ = events;
    postUrl_ = postUrl;
    throttleMs_ = std::max(0, throttleMs);
    backoffMinMs_ = std::max(50, backoffMinMs);
    backoffMaxMs_ = std::max(backoffMinMs_, backoffMaxMs);
    running_ = true;
    thread_ = std::thread(&HttpFacesPoster::run, this);
    return true;
}

void HttpFacesPoster::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void HttpFacesPoster::run() {
#ifdef _WIN32
    if (events_) events_->append("post_start", "url=[已脱敏]");

    std::uint64_t lastSeqSent = 0;
    std::uint64_t pendingSeq = 0;
    std::string pendingBody;

    auto lastSendT = std::chrono::steady_clock::now() - std::chrono::milliseconds(throttleMs_);
    int backoffMs = backoffMinMs_;

    while (running_) {
        std::uint64_t seq = 0;
        if (pipe_ && pipe_->waitFacesSeqChanged(pendingSeq ? pendingSeq : lastSeqSent, 500, seq)) {
            FacesSnapshot snap;
            if (pipe_ && pipe_->snapshotFaces(snap)) {
                pendingSeq = seq;
                pendingBody = buildFacesJson(snap);
            }
        }

        if (!running_) break;
        if (pendingSeq == 0 || pendingBody.empty()) continue;

        const auto now = std::chrono::steady_clock::now();
        if (throttleMs_ > 0 && now - lastSendT < std::chrono::milliseconds(throttleMs_)) continue;

        int status = 0;
        const bool ok = httpPostJson(postUrl_, pendingBody, 2000, status);
        if (ok) {
            lastSeqSent = pendingSeq;
            pendingSeq = 0;
            pendingBody.clear();
            lastSendT = now;
            backoffMs = backoffMinMs_;
            if (events_) events_->append("post_ok", "status=" + std::to_string(status));
        } else {
            if (events_) events_->append("post_fail", "status=" + std::to_string(status) + " backoff_ms=" + std::to_string(backoffMs));
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            backoffMs = std::min(backoffMaxMs_, backoffMs * 2);
        }
    }

    if (events_) events_->append("post_stop", "url=[已脱敏]");
#endif
}

}  // namespace rk_win

