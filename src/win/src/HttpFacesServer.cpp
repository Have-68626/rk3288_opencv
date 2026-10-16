#include "rk_win/HttpFacesServer.h"
#include "rk_win/HttpFacesServerPath.h"
#include "rk_win/StreamSession.h"

#include "rk_win/EndpointRegistry.h"

#include "rk_win/EventLogger.h"
#include "rk_win/FramePipeline.h"
#include "rk_win/JsonLite.h"
#include "rk_win/JsonEndpointHandlers.h"
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
#include <sstream>
#include <thread>

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
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "OK";
    }
}

// 恒定时间字符串比较（S-SEC-B09-001）：长度不同直接不等（长度本身属可接受信息泄漏）；
// 同长时逐字节 XOR 归并后一次判定，不提前退出，防基于首个差异字节的时序侧信道。
static bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

static bool pathHasDot(const std::string& path) {
    return path.find('.') != std::string::npos;
}

static bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

// S-SEC-B09-004：请求头读取（头名已小写存储，见 readRequest）。返回是否找到并赋值。
static bool getHeaderValue(const HttpFacesServer::HttpRequest& req, const char* name, std::string& out) {
    for (const auto& h : req.headers) {
        if (h.first == name) { out = h.second; return true; }
    }
    return false;
}

// 解析 Origin 头为 <scheme>://<host>[:<port>]，返回 host（小写）；scheme 非 http/https
// 或格式异常 → 返回空串（视为不允许）。忽略端口：本机回环任意端口均同机，属安全边界
// "同机器"，且兼容 Vite 开发代理（浏览器源为 localhost:<devPort>）。
static std::string originHost(const std::string& origin) {
    const auto sp = origin.find("://");
    if (sp == std::string::npos) return "";
    const std::string scheme = toLower(origin.substr(0, sp));
    if (scheme != "http" && scheme != "https") return "";
    std::string rest = origin.substr(sp + 3);
    for (const char cut : {'/', '?', '#'}) {
        const auto p = rest.find(cut);
        if (p != std::string::npos) rest = rest.substr(0, p);
    }
    // 端口剥离：IPv6 字面量 [::1]:port 仅去掉 ']' 之后的 :port；其余去掉最后一个 ':'。
    std::string host = rest;
    const auto bracket = host.find(']');
    const auto colon = host.rfind(':');
    if (colon != std::string::npos) {
        host = (bracket != std::string::npos && bracket < colon) ? host.substr(0, bracket + 1)
                                                                : host.substr(0, colon);
    }
    return toLower(host);
}

// 本机回环 host 白名单：精确匹配，杜绝 localhost.evil.com / 127.0.0.1.evil.com 子域绕过。
static bool isLoopbackHost(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

static bool isLoopbackOrigin(const std::string& origin) {
    return isLoopbackHost(originHost(origin));
}

// DNS rebinding 时浏览器的同站信号仍可能为真，所有路由都必须先验证实际请求 authority。
static bool hasLoopbackAuthority(const HttpFacesServer::HttpRequest& req) {
    const std::string* authority = nullptr;
    for (const auto& header : req.headers) {
        if (header.first != "host") continue;
        if (authority) return false;  // 重复 Host 由不同代理解释时可能产生不同的路由。
        authority = &header.second;
    }
    if (!authority || authority->empty()) return false;
    std::size_t end = authority->front() == '[' ? authority->find(']') : authority->find(':');
    if (authority->front() == '[') {
        if (end == std::string::npos) return false;
        ++end;
    } else if (end == std::string::npos) {
        end = authority->size();
    }
    if (!isLoopbackHost(toLower(authority->substr(0, end)))) return false;
    if (end == authority->size()) return true;
    if ((*authority)[end] != ':' || end + 1 == authority->size()) return false;
    unsigned int port = 0;
    for (std::size_t i = end + 1; i < authority->size(); ++i) {
        const char c = (*authority)[i];
        if (c < '0' || c > '9') return false;
        port = port * 10 + static_cast<unsigned int>(c - '0');
        if (port > 65535) return false;
    }
    return port != 0;
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

}  // namespace (匿名)

// ──── 构造函数 / 生命周期 ────────────────────────────────────

HttpFacesServer::HttpFacesServer()
    : streamRunner_(running_, stream::makeFrameProvider(&pipe_))
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

    // S-SEC-B09-001：Bearer token 来源 = 环境变量（配置字段 http.authToken 待 CONFIG 任务
    // 扩展，见 docs/security/auth-model.md §2）。空 = 认证未启用（兼容模式，规格 §3.3）。
    // token 仅存于进程内存，不写入日志与响应体（脱敏纪律 §6）。
    authToken_ = utf8FromWide(getEnvW(L"RK_WCFR_HTTP_AUTH_TOKEN"));

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
    // webroot 缺失时保留既有服务启动语义；静态读取会因未固定根句柄而 fail-closed。
    (void)staticRoot_.open(docRoot_);

    listenSock_ = static_cast<std::uintptr_t>(s);
    running_ = true;
    try {
        acceptThread_ = std::thread([this]() { acceptLoop(); });
    } catch (...) {
        running_ = false;
        closesocket(s);
        listenSock_ = 0;
        staticRoot_.close();
        return false;
    }
    return true;
}

void HttpFacesServer::stop() {
    if (!running_) {
        staticRoot_.close();
        return;
    }
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

    // 所有客户端线程已完成 → join 清理（回收残余线程/句柄，Q-CQ-B06-017）
    serverStopping_ = true;
    clientThreads_.joinAll();

    {
        std::lock_guard<std::mutex> lk3(clientMu_);
        clientSocks_.clear();
    }
    staticRoot_.close();
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
        std::shared_ptr<std::atomic<bool>> done;
        std::thread t;
        try {
            // S3 CONFIRMED：try 仅包住"线程创建"阶段（make_shared + std::thread 构造）。
            // 这两个操作可能抛 bad_alloc / std::system_error（线程资源不足）；
            // 此时线程尚未创建/入池，catch 回滚 activeClients_ 是安全的。
            //
            // 若像旧实现那样把 add()/reclaimFinished() 也包进 try，计数会被破坏：
            // add() 成功把线程入池后，reclaimFinished() 内部 vector push_back 抛 bad_alloc 时，
            // catch 会误把"已入池线程"当作创建失败，执行 activeClients_-- + closesocket；
            // 但线程仍在运行，其 Cleanup 析构还会再减一次 → 计数 0→1→0→-1，
            // stop() 等待 activeClients_==0 的谓词永不成立，导致关停挂起。
            // 故 add()/reclaimFinished() 移到 try 之外：其异常不触发回滚，计数不变量不被破坏。
            done = std::make_shared<std::atomic<bool>>(false);
            t = std::thread([this, cs, lease = std::move(*lease), done]() mutable {
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
                *done = true;   // 标记线程已完成：acceptLoop 据此 join 并释放线程句柄
            });
        } catch (const std::exception&) {
            // 仅"线程创建"失败才回滚：此时无线程被创建，还原连接状态，
            // 防止 activeClients_ 泄露和资源耗尽崩溃 (DoS)。
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
        // 线程入池与回收在 try 之外（S3 CONFIRMED）：add() 成功即拥有线程，
        // 其后的异常（理论仅 bad_alloc）不得触发 catch 回滚，否则会二次递减
        // activeClients_。t.joinable() 为 false 表示创建失败已回滚，跳过入池。
        if (t.joinable()) {
            clientThreads_.add(std::move(t), std::move(done));
            // 回收机制（Q-CQ-B06-017）：每次 accept 后立即 join 已结束的客户端线程，
            // 这样已完成连接的 std::thread 与 OS 线程句柄会被及时回收，
            // clientThreads_ 长期有界（≤ 并发连接上限），不会随请求总量线性增长。
            clientThreads_.reclaimFinished();
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

    std::size_t pos = 0;
    auto getLine = [&](std::string& lineOut) -> bool {
        if (pos >= head.size()) return false;
        std::size_t end = head.find('\n', pos);
        if (end == std::string::npos) {
            lineOut = head.substr(pos);
            pos = head.size();
        } else {
            lineOut = head.substr(pos, end - pos);
            pos = end + 1;
        }
        if (!lineOut.empty() && lineOut.back() == '\r') lineOut.pop_back();
        return true;
    };

    std::string requestLine;
    if (!getLine(requestLine)) {
        err = "bad_request_line";
        return false;
    }

    std::size_t methodEnd = requestLine.find_first_of(" \t");
    if (methodEnd == std::string::npos) {
        err = "bad_request_line";
        return false;
    }
    std::size_t targetStart = requestLine.find_first_not_of(" \t", methodEnd);
    if (targetStart == std::string::npos) {
        err = "bad_request_line";
        return false;
    }
    std::size_t targetEnd = requestLine.find_first_of(" \t", targetStart);
    if (targetEnd == std::string::npos) {
        err = "bad_request_line";
        return false;
    }
    std::size_t versionStart = requestLine.find_first_not_of(" \t", targetEnd);
    if (versionStart == std::string::npos) {
        err = "bad_request_line";
        return false;
    }

    out.method = requestLine.substr(0, methodEnd);
    std::string target = requestLine.substr(targetStart, targetEnd - targetStart);
    out.httpVersion = requestLine.substr(versionStart);

    if (out.method.empty() || target.empty()) {
        err = "bad_request_line";
        return false;
    }

    auto qpos = target.find('?');
    out.path = (qpos == std::string::npos) ? target : target.substr(0, qpos);
    out.query = (qpos == std::string::npos) ? "" : target.substr(qpos + 1);

    std::string line;
    while (getLine(line)) {
        if (line.empty()) continue;
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
    if (startsWith(req.path, "/api") || req.path == "/openapi.json") return handleApi(req);
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
    if (!staticRoot_.resolve(rel, p)) return jsonErr(400, "invalid_path", "路径不合法");

    std::string body;
    // S-SEC-B09-007：根目录句柄在启动时固定；读取时逐层相对父目录句柄打开，
    // 任一 symlink/junction 等重解析点均 fail-closed。
    if (!rk_win::readFileWithinRoot(staticRoot_, p, body)) {
        if (!pathHasDot(req.path)) {
            std::filesystem::path idx;
            if (staticRoot_.resolve("/index.html", idx) &&
                rk_win::readFileWithinRoot(staticRoot_, idx, body)) {
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

// ──── HTTP 认证授权（S-SEC-B09-001，规格见 docs/security/auth-model.md）────

bool HttpFacesServer::isAnonymousPath(const std::string& path) const {
    // 规范别名也属于管理 API；它不带 /api 前缀，不能落入静态文件匿名白名单。
    if (path == "/openapi.json") return false;
    // 非 /api 前缀 = 静态文件托管（写操作已被 handleStaticOrFallback 以 405 拒绝）→ 匿名。
    if (path.rfind("/api", 0) != 0) return true;
    // /api 下仅只读状态端点匿名（健康检查 + 加速状态，无敏感动作/凭据）。
    return path == "/api/v1/health" || path == "/api/v1/acceleration";
}

bool HttpFacesServer::requestAuthorized(const HttpRequest& req) const {
    // 未启用认证（未配置 token）= 兼容模式：全匿名放行（规格 §3.3 残余风险）。
    if (authToken_.empty()) return true;
    // 匿名白名单：静态文件 + 只读状态端点。
    if (isAnonymousPath(req.path)) return true;

    // 提取 Authorization 头的 Bearer token。
    std::string headerValue;
    for (const auto& h : req.headers) {
        if (h.first == "authorization") {
            headerValue = h.second;
            break;
        }
    }
    // scheme 大小写不敏感（RFC 6750）；token 按原文比较。
    std::string value = trim(headerValue);
    const auto sp = value.find(' ');
    if (sp == std::string::npos) return false;
    std::string scheme = value.substr(0, sp);
    std::string token = trim(value.substr(sp + 1));
    for (auto& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme != "bearer") return false;
    if (token.empty()) return false;

    // 恒定时间比较：防时序侧信道（规格 §4）。
    return constantTimeEquals(token, authToken_);
}

HttpFacesServer::HttpResponse HttpFacesServer::unauthorizedResponse() {
    // 与既有 jsonErr 格式一致（含安全响应头）；不发送 WWW-Authenticate，
    // 避免暴露 token 机制/触发认证握手循环（规格 §3.4）。不回显请求 token。
    return jsonErr(401, "unauthorized", "需要有效的认证令牌");
}

// S-SEC-B09-004：跨站请求伪造（CSRF）防护。
// 仅在认证通过之后、路由/流分发之前调用（handleClient）。决策树见
// docs/security/auth-model.md §3.5：
//  1) 写方法（POST/PUT/DELETE）才校验；读方法恒放行（无副作用）；
//  2) Sec-Fetch-Site: cross-site → 拒绝（现代浏览器强信号，不可被 JS 伪造）；
//  3) 旧浏览器无 Sec-Fetch-Site → 回退校验 Origin：
//     - Origin 缺失 → 放行（curl/API 客户端/旧浏览器写）；空值 → 拒绝(403)（无源上下文）；
//     - 字面量 "null"（沙箱 iframe / data: 等无源上下文）→ 拒绝；
//     - 非本机回环 host → 拒绝；http(s)://127.0.0.1|localhost|::1 → 放行。
//  4) Origin/Sec-Fetch-Site 均缺失 → 放行（与 SEC-02 兼容模式一致，不破坏 curl）。
HttpFacesServer::HttpResponse HttpFacesServer::crossSiteForbiddenResponse() {
    // 与 jsonErr 格式一致（含安全响应头）；不回显请求中的 Origin/token（脱敏纪律 §6）。
    return jsonErr(403, "cross_site_forbidden", "跨站请求被拒绝");
}

bool HttpFacesServer::csrfCheckPasses(const HttpRequest& req) const {
    // 读方法无副作用，不受 CSRF 影响（GET/HEAD 等一律放行）。
    if (req.method != "POST" && req.method != "PUT" && req.method != "DELETE") return true;

    // 1) Sec-Fetch-Site：cross-site → 拒绝；其余值（same-origin/same-site/none）→ 放行。
    std::string v;
    if (getHeaderValue(req, "sec-fetch-site", v)) {
        return toLower(v) != "cross-site";
    }
    // 2) 旧浏览器无 Sec-Fetch-Site：回退校验 Origin；缺失 → 放行（curl/API 客户端）。
    if (getHeaderValue(req, "origin", v)) {
        if (v.empty() || v == "null") return false;  // 无源上下文（沙箱 iframe/data:）跨站风险高
        return isLoopbackOrigin(v);
    }
    return true;
}

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

    // S-SEC-B09-001：认证授权。在流旁路与 handleRequest 分发之前统一收口校验，
    // 同时覆盖 EndpointRegistry 路由与 StreamSessionRunner 旁路两类请求，
    // 避免"注册表已锁但流漏放行"（规格 §4）。未通过 → 401，不执行任何 handler。
    if (!requestAuthorized(req)) {
        (void)writeResponse(sock, unauthorizedResponse());
        return;
    }

    // Host 校验覆盖读方法、静态资源和流，不能只用写方法的来源信号阻止 DNS rebinding。
    if (!hasLoopbackAuthority(req)) {
        (void)writeResponse(sock, crossSiteForbiddenResponse());
        return;
    }

    // S-SEC-B09-004：跨站请求伪造（CSRF）防护。认证通过之后、路由/流分发之前收口：
    // 对写方法（POST/PUT/DELETE）校验 Origin / Sec-Fetch-Site（决策树见 §3.5）。
    // 拒绝 → 403，不执行任何 handler（写操作零副作用，含匿名兼容模式）。
    if (!csrfCheckPasses(req)) {
        (void)writeResponse(sock, crossSiteForbiddenResponse());
        return;
    }

    // 流式端点统一：StreamSessionRunner 接管 socket 生命周期
    if (stream::isStreamRequest(req.path, req.method)) {
        const StreamType st = (req.path == "/api/v1/preview.mjpeg")
                                  ? StreamType::Mjpeg
                                  : StreamType::Sse;
        streamRunner_.run(sock, st);
        return;
    }

    const HttpResponse resp = handleRequest(req);
    (void)writeResponse(sock, resp);
}

}  // namespace rk_win
