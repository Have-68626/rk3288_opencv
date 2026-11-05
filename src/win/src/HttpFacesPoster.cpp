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
#else
// 决策 D6：Linux 上报使用 libcurl（HTTPS、代理、超时齐全），禁用系统代理以对齐 Windows 的 NO_PROXY。
#include <curl/curl.h>
#include <strings.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

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
#else
// Linux：URL 校验与 libcurl 发送。语义对齐 Windows 侧：只允许 http/https、拒绝控制字符与空白、
// 不使用系统代理、四项超时同值、2xx 视为成功。
bool isValidDestination(const std::string& value) {
    if (value.empty() || std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c <= 0x20 || c == 0x7f;
    })) return false;
    CURLU* u = curl_url();
    if (!u) return false;
    bool ok = false;
    if (curl_url_set(u, CURLUPART_URL, value.c_str(), 0) == CURLUE_OK) {
        char* scheme = nullptr;
        char* host = nullptr;
        if (curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
            curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
            scheme && host && *host &&
            (strcasecmp(scheme, "http") == 0 || strcasecmp(scheme, "https") == 0)) {
            ok = true;
        }
        if (scheme) curl_free(scheme);
        if (host) curl_free(host);
    }
    curl_url_cleanup(u);
    return ok;
}

std::once_flag g_curlOnce;
std::size_t discardBody(char*, std::size_t size, std::size_t n, void*) { return size * n; }

bool httpPostJson(const std::string& urlUtf8, const std::string& body, int timeoutMs, int& statusOut) {
    statusOut = 0;
    std::call_once(g_curlOnce, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* h = curl_easy_init();
    if (!h) return false;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json; charset=utf-8");
    curl_easy_setopt(h, CURLOPT_URL, urlUtf8.c_str());
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, discardBody);
    const CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    statusOut = static_cast<int>(status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    return rc == CURLE_OK && status >= 200 && status < 300;
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
    if (!isValidDestination(postUrl)) return false;
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
}

}  // namespace rk_win

