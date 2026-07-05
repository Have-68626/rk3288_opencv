#include "rk_win/HttpFacesServer.h"
#include "rk_win/HttpFacesServerPath.h"

#include "rk_win/EventLogger.h"
#include "rk_win/FramePipeline.h"
#include "rk_win/JsonLite.h"
#include "rk_win/WinConfig.h"
#include "rk_win/WinJsonConfig.h"


#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <sstream>
#include <thread>

#if RK_WIN_HAS_OPENCV
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace rk_win {
namespace {

// ──── 命名常量（替代散落魔法数字）───────────────────────────
constexpr int kRecvBufSize = 2048;
constexpr std::size_t kMaxHeaderSize = 64 * 1024;
constexpr std::size_t kMaxBodySize = 1024 * 1024;
constexpr std::size_t kSendChunkSize = 1u << 20;   // 1 MiB
#ifdef _WIN32
constexpr DWORD kSocketTimeoutMs = 5000;
#else
constexpr int kSocketTimeoutMs = 5000;
#endif
constexpr int kStaticCacheMaxAge = 31536000;        // 1 year
constexpr int kAcceptRetryMs = 50;

// RAII 封装 SOCKET: 析构时自动 closesocket, 消除手动关闭遗漏风险
struct SocketGuard {
    SOCKET s = INVALID_SOCKET;
    explicit SocketGuard(SOCKET s_) : s(s_) {}
    ~SocketGuard() { if (s != INVALID_SOCKET) closesocket(s); }
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    SocketGuard(SocketGuard&& other) noexcept : s(other.s) { other.s = INVALID_SOCKET; }
    SocketGuard& operator=(SocketGuard&& other) noexcept { if (this != &other) { if (s != INVALID_SOCKET) closesocket(s); s = other.s; other.s = INVALID_SOCKET; } return *this; }
    void release() { s = INVALID_SOCKET; }
};

struct WsaGuard {
    bool ok = false;
    WsaGuard() {
#ifdef _WIN32
        WSADATA wsa;
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    }
    ~WsaGuard() {
#ifdef _WIN32
        if (ok) WSACleanup();
#endif
    }
};

static std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string trim(std::string s) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

static std::string statusReason(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "OK";
    }
}

static bool pathHasDot(const std::string& path) {
    return path.find('.') != std::string::npos;
}

static bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

static std::string guessContentType(const std::string& path) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return "application/octet-stream";
    const std::string ext = toLower(path.substr(pos + 1));
    if (ext == "html") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "js") return "application/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "ico") return "image/x-icon";
    if (ext == "map") return "application/json; charset=utf-8";
    return "application/octet-stream";
}

static bool readFileBinary(const std::filesystem::path& p, std::string& out) {
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const auto sz = ifs.tellg();
    if (sz <= 0) { out.clear(); return true; }
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(sz));
    ifs.read(out.data(), static_cast<std::streamsize>(sz));
    return true;
}


}  // namespace

}  // namespace

// ──── 构造函数 / 生命周期 ────────────────────────────────────

HttpFacesServer::HttpFacesServer()
    : streamRunner_(running_, [this](cv::Mat& out) -> bool {
          if (!pipe_) return false;  // 防御：pipe_ 尚未注入（在 start() 中设置）
          RenderState rs;
          if (!pipe_->tryGetRenderState(rs)) return false;
          out = rs.bgr;
          return true;
      })
    , registry_(std::make_unique<EndpointRegistry>()) {
    handlers::registerAllHttpApi(*registry_);
}

HttpFacesServer::~HttpFacesServer() {
    stop();
}

bool HttpFacesServer::start(FramePipeline* pipe, EventLogger* events, int port, WinJsonConfigStore* settings) {
    if (running_) return true;
    pipe_ = pipe;
    streamRunner_.setPipe(pipe);
    events_ = events;
    settings_ = settings;
    port_ = port;

    docRoot_ = resolvePathFromExeDir(L"webroot");

    static WsaGuard wsa;
    if (!wsa.ok) return false;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    const int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return false;
    }
    if (listen(s, SOMAXCONN) != 0) {
        closesocket(s);
        return false;
    }

    listenSock_ = static_cast<std::uintptr_t>(s);
    running_ = true;
    acceptThread_ = std::thread([this]() { acceptLoop(); });
    return true;
}

void HttpFacesServer::stop() {
    if (!running_) return;
    running_ = false;

    const auto ls = static_cast<SOCKET>(listenSock_);
    if (ls != 0) {
        closesocket(ls);
        listenSock_ = 0;
    }

    if (acceptThread_.joinable()) acceptThread_.join();

    std::vector<std::uintptr_t> socks;
    {
        std::lock_guard<std::mutex> lk(clientMu_);
        socks = clientSocks_;
    }
    for (const auto sp : socks) {
        const SOCKET s = static_cast<SOCKET>(sp);
        shutdown(s, SD_BOTH);
    }

    std::unique_lock<std::mutex> lk(stopMu_);
    stopCv_.wait(lk, [this]() { return activeClients_.load() == 0; });

    // 所有客户端线程已完成 → join 清理
    serverStopping_ = true;
    {
        std::lock_guard<std::mutex> lk2(clientThreadsMu_);
        for (auto& t : clientThreads_) {
            if (t.joinable()) t.join();
        }
        clientThreads_.clear();
    }

    {
        std::lock_guard<std::mutex> lk3(clientMu_);
        clientSocks_.clear();
    }
}

// ──── 连接 / I/O ────────────────────────────────────────────

void HttpFacesServer::acceptLoop() {
    const SOCKET ls = static_cast<SOCKET>(listenSock_);
    while (running_) {
        sockaddr_in caddr{};
        int clen = sizeof(caddr);
        SOCKET cs = accept(ls, reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (cs == INVALID_SOCKET) {
            if (!running_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(kAcceptRetryMs));
            continue;
        }

        auto lease = quota_.tryAcquire();
        if (!lease.has_value()) {
            closesocket(cs);
            continue;
        }

#ifdef _WIN32
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&kSocketTimeoutMs), sizeof(kSocketTimeoutMs));
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&kSocketTimeoutMs), sizeof(kSocketTimeoutMs));
#else
        struct timeval tv;
        tv.tv_sec = kSocketTimeoutMs / 1000;
        tv.tv_usec = (kSocketTimeoutMs % 1000) * 1000;
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

        const auto sockP = static_cast<std::uintptr_t>(cs);
        {
            std::lock_guard<std::mutex> lk(clientMu_);
            clientSocks_.push_back(sockP);
        }

        activeClients_++;
        try {
            auto running = std::make_shared<std::atomic<bool>>(true);
            std::thread t([this, cs, lease = std::move(*lease), running]() mutable {
                struct Cleanup {
                    HttpFacesServer* self;
                    std::uintptr_t sockP;
                    ~Cleanup() {
                        {
                            std::lock_guard<std::mutex> lk(self->clientMu_);
                            for (auto it = self->clientSocks_.begin(); it != self->clientSocks_.end(); ++it) {
                                if (*it == sockP) {
                                    self->clientSocks_.erase(it);
                                    break;
                                }
                            }
                        }
                        self->activeClients_--;
                        self->stopCv_.notify_all();
                    }
                } cleanup{this, static_cast<std::uintptr_t>(cs)};

                handleClient(static_cast<std::uintptr_t>(cs));
                *running = false;
            });
            {
                std::lock_guard<std::mutex> lk(clientThreadsMu_);
                clientThreads_.push_back(std::move(t));
            }
        } catch (const std::exception&) {
            // 线程创建失败：回滚连接状态，防止 activeClients_ 泄露和资源耗尽崩溃 (DoS)
            {
                std::lock_guard<std::mutex> lk(clientMu_);
                for (auto it = clientSocks_.begin(); it != clientSocks_.end(); ++it) {
                    if (*it == sockP) {
                        clientSocks_.erase(it);
                        break;
                    }
                }
            }
            activeClients_--;
            stopCv_.notify_all();
            closesocket(cs);
        }
    }
}

bool HttpFacesServer::readRequest(std::uintptr_t sock, HttpRequest& out, std::string& err) {
    err.clear();
    out = HttpRequest{};

    std::string buf;
    buf.reserve(8192);
    char tmp[kRecvBufSize];
    const SOCKET s = static_cast<SOCKET>(sock);

    auto findHeaderEnd = [&]() -> std::size_t { return buf.find("\r\n\r\n"); };
    while (buf.size() < kMaxHeaderSize) {
        const int r = recv(s, tmp, static_cast<int>(sizeof(tmp)), 0);
        if (r <= 0) {
            err = "recv_failed";
            return false;
        }
        buf.append(tmp, tmp + r);
        if (findHeaderEnd() != std::string::npos) break;
    }
    const auto headerEnd = findHeaderEnd();
    if (headerEnd == std::string::npos) {
        err = "headers_too_large_or_incomplete";
        return false;
    }

    const std::string head = buf.substr(0, headerEnd);
    std::istringstream hs(head);
    std::string requestLine;
    if (!std::getline(hs, requestLine)) {
        err = "bad_request_line";
        return false;
    }
    if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();
    std::istringstream rl(requestLine);
    std::string target;
    rl >> out.method >> target >> out.httpVersion;
    if (out.method.empty() || target.empty()) {
        err = "bad_request_line";
        return false;
    }

    auto qpos = target.find('?');
    out.path = (qpos == std::string::npos) ? target : target.substr(0, qpos);
    out.query = (qpos == std::string::npos) ? "" : target.substr(qpos + 1);

    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto p = line.find(':');
        if (p == std::string::npos) continue;
        std::string k = toLower(trim(line.substr(0, p)));
        std::string v = trim(line.substr(p + 1));
        out.headers.emplace_back(std::move(k), std::move(v));
    }

    std::size_t contentLen = 0;
    for (const auto& kv : out.headers) {
        if (kv.first == "content-length") {
            contentLen = static_cast<std::size_t>(std::strtoull(kv.second.c_str(), nullptr, 10));
            break;
        }
    }
    if (contentLen > kMaxBodySize) {
        err = "body_too_large";
        return false;
    }

    const std::size_t bodyStart = headerEnd + 4;
    while (buf.size() - bodyStart < contentLen) {
        const int r = recv(s, tmp, static_cast<int>(sizeof(tmp)), 0);
        if (r <= 0) {
            err = "recv_body_failed";
            return false;
        }
        buf.append(tmp, tmp + r);
    }

    if (contentLen) out.body = buf.substr(bodyStart, contentLen);
    return true;
}

bool HttpFacesServer::writeRaw(std::uintptr_t sock, const void* data, std::size_t n) {
    const SOCKET s = static_cast<SOCKET>(sock);
    const char* p = static_cast<const char*>(data);
    std::size_t left = n;
    while (left > 0) {
        const int w = send(s, p, static_cast<int>(std::min<std::size_t>(left, kSendChunkSize)), 0);
        if (w <= 0) return false;
        p += w;
        left -= static_cast<std::size_t>(w);
    }
    return true;
}

bool HttpFacesServer::writeResponse(std::uintptr_t sock, const HttpResponse& resp) {
    std::string head;
    head.reserve(256 + resp.headers.size() * 64);
    head += "HTTP/1.1 ";
    head += std::to_string(resp.status);
    head += " ";
    head += resp.reason.empty() ? statusReason(resp.status) : resp.reason;
    head += "\r\nContent-Type: ";
    head += resp.contentType;
    head += "\r\nContent-Length: ";
    head += std::to_string(resp.body.size());
    head += "\r\nConnection: ";
    head += resp.close ? "close" : "keep-alive";
    head += "\r\n";
    for (const auto& h : resp.headers) {
        head += h.first;
        head += ": ";
        head += h.second;
        head += "\r\n";
    }
    head += "\r\n";
    if (!writeRaw(sock, head.data(), head.size())) return false;
    if (!resp.body.empty() && !writeRaw(sock, resp.body.data(), resp.body.size())) return false;
    return true;
}

// ──── 响应构建 ───────────────────────────────────────────────

HttpFacesServer::HttpResponse HttpFacesServer::jsonOk(const std::string& bodyJson) {
    HttpResponse r;
    r.status = 200;
    r.reason = "OK";
    r.contentType = "application/json; charset=utf-8";
    r.body = bodyJson;
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    r.headers.push_back({"Content-Security-Policy", "default-src 'none'"});
    return r;
}

HttpFacesServer::HttpResponse HttpFacesServer::jsonOk(JsonValue data) {
    JsonValue root = JsonValue::makeObject();
    root.o["ok"] = JsonValue::makeBool(true);
    root.o["data"] = std::move(data);
    return jsonOk(toJsonString(root, false));
}

HttpFacesServer::HttpResponse HttpFacesServer::jsonErr(int httpStatus, const std::string& code, const std::string& message,
                                                       const std::vector<std::string>& details) {
    JsonValue root = JsonValue::makeObject();
    root.o["ok"] = JsonValue::makeBool(false);
    JsonValue err = JsonValue::makeObject();
    err.o["code"] = JsonValue::makeString(code);
    err.o["message"] = JsonValue::makeString(message);
    if (!details.empty()) {
        JsonValue d = JsonValue::makeArray();
        for (const auto& x : details) d.a.push_back(JsonValue::makeString(x));
        err.o["details"] = std::move(d);
    }
    root.o["error"] = std::move(err);

    HttpResponse r;
    r.status = httpStatus;
    r.reason = statusReason(httpStatus);
    r.contentType = "application/json; charset=utf-8";
    r.body = toJsonString(root, false);
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    r.headers.push_back({"Content-Security-Policy", "default-src 'none'"});
    return r;
}

// ──── 路由调度 ───────────────────────────────────────────────

HttpFacesServer::HttpResponse HttpFacesServer::handleRequest(const HttpRequest& req) {
    if (startsWith(req.path, "/api")) return handleApi(req);
    return handleStaticOrFallback(req);
}

HttpFacesServer::HttpResponse HttpFacesServer::handleApi(const HttpRequest& req) {
    EndpointContext ctx;
    ctx.pipe = pipe_;
    ctx.settings = settings_;
    ctx.port = port_;
    return registry_->dispatch(req, ctx);
}

HttpFacesServer::HttpResponse HttpFacesServer::handleStaticOrFallback(const HttpRequest& req) {
    if (req.method != "GET") return jsonErr(405, "method_not_allowed", "仅支持 GET");

    std::string rel = req.path;
    if (rel == "/") rel = "/index.html";

    std::filesystem::path p;
    if (!rk_win::isSafeRelativePath(docRoot_, rel, p)) return jsonErr(400, "invalid_path", "路径不合法");

    std::string body;
    if (!readFileBinary(p, body)) {
        if (!pathHasDot(req.path)) {
            std::filesystem::path idx = docRoot_ / "index.html";
            if (readFileBinary(idx, body)) {
                HttpResponse r;
                r.status = 200;
                r.reason = "OK";
                r.contentType = "text/html; charset=utf-8";
                r.body = std::move(body);
                r.headers.push_back({"Cache-Control", "no-cache"});
                r.headers.push_back({"X-Content-Type-Options", "nosniff"});
                r.headers.push_back({"X-Frame-Options", "DENY"});
                return r;
            }
        }
        return jsonErr(404, "not_found", "文件不存在");
    }

    HttpResponse r;
    r.status = 200;
    r.reason = "OK";
    r.contentType = guessContentType(rel);
    r.body = std::move(body);
    if (startsWith(rel, "/assets/") || rel.find(".js") != std::string::npos || rel.find(".css") != std::string::npos) {
        r.headers.push_back({"Cache-Control", "public, max-age=" + std::to_string(kStaticCacheMaxAge) + ", immutable"});
    } else {
        r.headers.push_back({"Cache-Control", "no-cache"});
    }
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    return r;
}

// ──── 客户端处理 ─────────────────────────────────────────────

// ──── 客户端处理 ─────────────────────────────────────────────

void HttpFacesServer::handleClient(std::uintptr_t sock) {
    SocketGuard sg(static_cast<SOCKET>(sock));
    HttpRequest req;
    std::string err;

    if (!readRequest(sock, req, err)) {
        HttpResponse r;
        r.status = (err == "body_too_large") ? 413 : 400;
        r.reason = statusReason(r.status);
        r.contentType = "application/json; charset=utf-8";
        r.body = jsonErr(r.status, "invalid_request", "请求解析失败", {err}).body;
        (void)writeResponse(sock, r);
        return;
    }

    // 流式端点统一：StreamSessionRunner 接管 socket 生命周期
    if ((req.path == "/api/faces/stream" || req.path == "/api/v1/faces/stream") && req.method == "GET") {
        streamRunner_.run(sock, StreamType::Sse);
        return;
    }

    if (req.path == "/api/v1/preview.mjpeg" && req.method == "GET") {
        streamRunner_.run(sock, StreamType::Mjpeg);
        return;
    }

    const HttpResponse resp = handleRequest(req);
    (void)writeResponse(sock, resp);
}

}  // namespace rk_win
