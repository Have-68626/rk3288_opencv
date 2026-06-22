#include "rk_win/HttpFacesServer.h"
#include "rk_win/HttpFacesServerPath.h"

#include "rk_win/EventLogger.h"
#include "rk_win/FacesJson.h"
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
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}


static std::string escapeJsonString(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

static std::string utf8FromWideLocal(const std::wstring& ws) {
    if (ws.empty()) return {};
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
#else
    return std::string(ws.begin(), ws.end());
#endif
}

#if RK_WIN_HAS_OPENCV
static bool buildJpegWithOverlay(RenderState& rs, std::vector<std::uint8_t>& outJpeg) {
    if (rs.bgr.empty()) return false;
    for (const auto& f : rs.faces) {
        cv::rectangle(rs.bgr, f.rect, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
    }
    static const std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 80};
    std::vector<uchar> buf;
    if (!cv::imencode(".jpg", rs.bgr, buf, params)) return false;
    outJpeg.assign(buf.begin(), buf.end());
    return true;
}
#endif

}  // namespace

// ──── SSE 流会话 ────────────────────────────────────────────
namespace {
class SseSession : public HttpFacesServer::StreamSession {
public:
    std::string contentType() const override { return "text/event-stream; charset=utf-8"; }
    int idleMs() const override { return 100; }

private:
    std::uint64_t lastSeq_ = 0;
    std::chrono::steady_clock::time_point lastKeep_ = std::chrono::steady_clock::now();

public:
    bool writeFrame(std::uintptr_t sock, FramePipeline* pipe, bool& running) override {
        auto* self = reinterpret_cast<HttpFacesServer*>(this);
        (void)self;

        std::uint64_t seq = 0;
        const bool changed = pipe ? pipe->waitFacesSeqChanged(lastSeq_, 1000, seq) : false;
        if (!running) return false;
        if (changed) {
            lastSeq_ = seq;
            FacesSnapshot snap;
            if (pipe && pipe->snapshotFaces(snap)) {
                const std::string body = buildFacesJson(snap);
                std::string evt = "data: " + body + "\n\n";
                return self->writeRaw(sock, evt.data(), evt.size());
            }
        } else {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastKeep_ > std::chrono::seconds(10)) {
                const std::string ka = ": keepalive\n\n";
                lastKeep_ = now;
                return self->writeRaw(sock, ka.data(), ka.size());
            }
        }
        return true;
    }
};

class MjpegSession : public HttpFacesServer::StreamSession {
public:
    std::string contentType() const override {
        return "multipart/x-mixed-replace; boundary=rk_boundary";
    }
    int idleMs() const override { return 100; }

    bool writeFrame(std::uintptr_t sock, FramePipeline* pipe, bool& running) override {
#if RK_WIN_HAS_OPENCV
        auto* self = reinterpret_cast<HttpFacesServer*>(this);
        if (!pipe) return false;
        RenderState rs;
        if (!pipe->tryGetRenderState(rs)) return true;
        std::vector<std::uint8_t> jpeg;
        if (!buildJpegWithOverlay(rs, jpeg)) return true;
        std::ostringstream os;
        os << "--rk_boundary\r\n"
           << "Content-Type: image/jpeg\r\n"
           << "Content-Length: " << jpeg.size() << "\r\n"
           << "\r\n";
        const std::string part = os.str();
        if (!self->writeRaw(sock, part.data(), part.size())) return false;
        if (!self->writeRaw(sock, jpeg.data(), jpeg.size())) return false;
        if (!self->writeRaw(sock, "\r\n", 2)) return false;
#else
        (void)sock; (void)pipe; (void)running;
#endif
        return true;
    }
};
}  // namespace

// ──── 端点路由表 ─────────────────────────────────────────────
static const HttpFacesServer::Route kRoutes[] = {
    {"/api/v1/health",             "GET",  &HttpFacesServer::onHealth},
    {"/api/v1/models",             "GET",  &HttpFacesServer::onModels},
    {"/api/v1/models/reload",      "POST", &HttpFacesServer::onModelReload},
    {"/api/v1/acceleration",       "GET",  &HttpFacesServer::onAcceleration},
    {"/api/v1/openapi",            "GET",  &HttpFacesServer::onOpenApi},
    {"/openapi.json",              "GET",  &HttpFacesServer::onOpenApi},
    {"/api/v1/settings",           "*",    &HttpFacesServer::onSettings},
    {"/api/v1/actions/crypto/rotate", "POST", &HttpFacesServer::onCryptoRotate},
    {"/api/faces",                 "GET",  &HttpFacesServer::onFaces},
    {"/api/v1/faces",              "GET",  &HttpFacesServer::onFaces},
    {"/api/v1/cameras",            "GET",  &HttpFacesServer::onCameras},
    {"/api/v1/camera/flip",        "PUT",  &HttpFacesServer::onCameraFlip},
    {"/api/v1/actions/enroll",     "POST", &HttpFacesServer::onEnroll},
    {"/api/v1/actions/db/clear",   "POST", &HttpFacesServer::onDbClear},
    {"/api/v1/actions/privacy/open", "POST", &HttpFacesServer::onPrivacyOpen},
    {"/api/v1/preview.jpg",        "GET",  &HttpFacesServer::onPreviewJpg},
};

// ──── 构造函数 / 生命周期 ────────────────────────────────────

HttpFacesServer::HttpFacesServer() = default;

HttpFacesServer::~HttpFacesServer() {
    stop();
}

bool HttpFacesServer::start(FramePipeline* pipe, EventLogger* events, int port, WinJsonConfigStore* settings) {
    if (running_) return true;
    pipe_ = pipe;
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

    {
        std::lock_guard<std::mutex> lk2(clientMu_);
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
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        auto lease = quota_.tryAcquire();
        if (!lease.has_value()) {
            closesocket(cs);
            continue;
        }

#ifdef _WIN32
        DWORD timeout = 5000;
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

        const auto sockP = static_cast<std::uintptr_t>(cs);
        {
            std::lock_guard<std::mutex> lk(clientMu_);
            clientSocks_.push_back(sockP);
        }

        activeClients_++;
        std::thread([this, cs, lease = std::move(*lease)]() mutable {
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
        }).detach();
    }
}

bool HttpFacesServer::readRequest(std::uintptr_t sock, HttpRequest& out, std::string& err) {
    err.clear();
    out = HttpRequest{};

    std::string buf;
    buf.reserve(8192);
    char tmp[2048];
    const SOCKET s = static_cast<SOCKET>(sock);

    auto findHeaderEnd = [&]() -> std::size_t { return buf.find("\r\n\r\n"); };
    while (buf.size() < 64 * 1024) {
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
    if (contentLen > 1024 * 1024) {
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
        const int w = send(s, p, static_cast<int>(std::min<std::size_t>(left, 1u << 20)), 0);
        if (w <= 0) return false;
        p += w;
        left -= static_cast<std::size_t>(w);
    }
    return true;
}

bool HttpFacesServer::writeResponse(std::uintptr_t sock, const HttpResponse& resp) {
    std::ostringstream os;
    os << "HTTP/1.1 " << resp.status << " " << (resp.reason.empty() ? statusReason(resp.status) : resp.reason) << "\r\n";
    os << "Content-Type: " << resp.contentType << "\r\n";
    os << "Content-Length: " << resp.body.size() << "\r\n";
    os << "Connection: " << (resp.close ? "close" : "keep-alive") << "\r\n";
    for (const auto& h : resp.headers) {
        os << h.first << ": " << h.second << "\r\n";
    }
    os << "\r\n";
    const std::string head = os.str();
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
    for (const auto& r : kRoutes) {
        if (req.path == r.path) {
            if (r.method[0] != '*' && req.method != r.method) {
                return jsonErr(405, "method_not_allowed", "仅支持 " + std::string(r.method));
            }
            return (this->*r.handler)(req);
        }
    }
    return jsonErr(404, "not_found", "未知端点");
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
        r.headers.push_back({"Cache-Control", "public, max-age=31536000, immutable"});
    } else {
        r.headers.push_back({"Cache-Control", "no-cache"});
    }
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    return r;
}

// ──── 端点 Handler ───────────────────────────────────────────

HttpFacesServer::HttpResponse HttpFacesServer::onHealth(const HttpRequest&) {
    JsonValue d = JsonValue::makeObject();
    d.o["name"] = JsonValue::makeString("rk_wcfr");
    d.o["port"] = JsonValue::makeNumber(port_);
    return jsonOk(std::move(d));
}

HttpFacesServer::HttpResponse HttpFacesServer::onModels(const HttpRequest&) {
    JsonValue d = JsonValue::makeObject();

    JsonValue supported = JsonValue::makeArray();
    {
        JsonValue s1 = JsonValue::makeObject();
        s1.o["id"] = JsonValue::makeString("cascade_frontalface");
        s1.o["displayName"] = JsonValue::makeString("Cascade Frontal Face (LBP)");
        s1.o["taskType"] = JsonValue::makeString("detect_recognize_pipeline");
        s1.o["notes"] = JsonValue::makeString("LBP 级联分类器，极轻量。适合低资源环境，精度低于 DNN 方案。Windows 管线默认检测器。");
        s1.o["recommendedFor"] = JsonValue::makeString("high_speed");
        supported.a.push_back(std::move(s1));

        JsonValue s2 = JsonValue::makeObject();
        s2.o["id"] = JsonValue::makeString("dnn_face_detector");
        s2.o["displayName"] = JsonValue::makeString("OpenCV DNN Face Detector");
        s2.o["taskType"] = JsonValue::makeString("detect");
        s2.o["notes"] = JsonValue::makeString("ResNet SSD 300x300，OpenCV DNN 后端检测器。精度高于 Cascade，适合 Windows 管线。");
        s2.o["recommendedFor"] = JsonValue::makeString("balanced");
        supported.a.push_back(std::move(s2));
    }
    d.o["supportedModels"] = std::move(supported);

    JsonValue active = JsonValue::makeArray();
    int totalConfigured = 0;
    int totalLoaded = 0;
    int totalFailed = 0;
    int totalMissing = 0;

    if (pipe_) {
        std::vector<ModelSnapshot> models = pipe_->getActiveModels();
        totalConfigured = models.size();
        for (const auto& mSnap : models) {
            if (mSnap.status == "loaded") totalLoaded++;
            else if (mSnap.status == "failed") totalFailed++;
            else if (mSnap.status == "missing") totalMissing++;

            JsonValue am = JsonValue::makeObject();
            am.o["id"] = JsonValue::makeString(mSnap.id);
            am.o["displayName"] = JsonValue::makeString(mSnap.displayName);
            am.o["taskType"] = JsonValue::makeString(mSnap.taskType);
            am.o["configuredPath"] = JsonValue::makeString(mSnap.configuredPath);
            am.o["resolvedPath"] = JsonValue::makeString(mSnap.resolvedPath);
            am.o["backend"] = JsonValue::makeString(mSnap.backend);
            if (!mSnap.hash.empty()) am.o["hash"] = JsonValue::makeString(mSnap.hash);
            am.o["status"] = JsonValue::makeString(mSnap.status);
            am.o["isInUse"] = JsonValue::makeBool(mSnap.isInUse);
            if (!mSnap.modelVersion.empty()) am.o["modelVersion"] = JsonValue::makeString(mSnap.modelVersion);
            if (!mSnap.lastError.empty()) am.o["lastError"] = JsonValue::makeString(mSnap.lastError);
            active.a.push_back(std::move(am));
        }
    }
    d.o["activeModels"] = std::move(active);

    JsonValue summary = JsonValue::makeObject();
    summary.o["totalSupported"] = JsonValue::makeNumber(2);
    summary.o["totalConfigured"] = JsonValue::makeNumber(totalConfigured);
    summary.o["totalLoaded"] = JsonValue::makeNumber(totalLoaded);
    summary.o["totalFailed"] = JsonValue::makeNumber(totalFailed);
    summary.o["totalMissing"] = JsonValue::makeNumber(totalMissing);
    d.o["summary"] = std::move(summary);

    return jsonOk(std::move(d));
}

HttpFacesServer::HttpResponse HttpFacesServer::onModelReload(const HttpRequest& req) {
    JsonValue body;
    std::string perr;
    if (!parseJson(req.body, body, perr) || !body.isObject()) {
        return jsonErr(400, "invalid_json", "请求体需为 JSON object");
    }
    const std::string* id = nullptr;
    if (const JsonValue* idv = body.find("id"); idv && idv->isString()) {
        id = &idv->s;
    }
    if (!id || id->empty()) {
        return jsonErr(400, "missing_id", "缺少 id 字段");
    }
    std::string status = "not_found";
    if (pipe_) {
        auto models = pipe_->getActiveModels();
        for (const auto& m : models) {
            if (m.id == *id) {
                status = "reload_requested";
                break;
            }
        }
    }
    JsonValue o = JsonValue::makeObject();
    o.o["id"] = JsonValue::makeString(*id);
    o.o["status"] = JsonValue::makeString(status);
    return jsonOk(std::move(o));
}

HttpFacesServer::HttpResponse HttpFacesServer::onAcceleration(const HttpRequest&) {
    JsonValue d = JsonValue::makeObject();

    if (settings_) {
        std::string redacted = settings_->currentRedactedJsonPretty();
        JsonValue settingsDoc;
        std::string perr;
        if (parseJson(redacted, settingsDoc, perr) && settingsDoc.isObject()) {
            if (const JsonValue* accel = settingsDoc.find("acceleration"); accel && accel->isObject()) {
                d.o["config"] = *accel;
            }
            if (const JsonValue* dnn = settingsDoc.find("dnn"); dnn && dnn->isObject()) {
                JsonValue dnnSummary = JsonValue::makeObject();
                if (const JsonValue* e = dnn->find("enable")) dnnSummary.o["enable"] = *e;
                if (const JsonValue* b = dnn->find("backend")) dnnSummary.o["backend"] = *b;
                if (const JsonValue* bt = dnn->find("confThreshold")) dnnSummary.o["confThreshold"] = *bt;
                d.o["dnn"] = std::move(dnnSummary);
            }
        }
    }

    JsonValue backends = JsonValue::makeArray();
    if (pipe_) {
        auto models = pipe_->getActiveModels();
        for (const auto& m : models) {
            if (m.isInUse) {
                JsonValue b = JsonValue::makeObject();
                b.o["id"] = JsonValue::makeString(m.id);
                b.o["backend"] = JsonValue::makeString(m.backend);
                b.o["status"] = JsonValue::makeString(m.status);
                backends.a.push_back(std::move(b));
            }
        }
    }
    d.o["activeBackends"] = std::move(backends);

    return jsonOk(std::move(d));
}

HttpFacesServer::HttpResponse HttpFacesServer::onOpenApi(const HttpRequest&) {
    JsonValue d = JsonValue::makeObject();
    d.o["note"] = JsonValue::makeString("OpenAPI 文档见 docs/windows-web-spa/openapi.yaml；此端点为最小联调占位");
    return jsonOk(std::move(d));
}

HttpFacesServer::HttpResponse HttpFacesServer::onSettings(const HttpRequest& req) {
    if (!settings_) return jsonErr(503, "settings_unavailable", "settings 存储未初始化");
    if (req.method == "GET") {
        const std::string redacted = settings_->currentRedactedJsonPretty();
        std::ostringstream os;
        os << "{\"ok\":true,\"data\":" << redacted << "}";
        return jsonOk(os.str());
    }
    if (req.method == "PUT") {
        const auto res = settings_->updateFromJsonBody(req.body);
        if (!res.ok) return jsonErr(res.httpStatus, res.code.empty() ? "invalid_request" : res.code, res.message, res.details);
        const std::string redacted = settings_->currentRedactedJsonPretty();
        std::ostringstream os;
        os << "{\"ok\":true,\"data\":" << redacted << "}";
        return jsonOk(os.str());
    }
    return jsonErr(405, "method_not_allowed", "仅支持 GET/PUT");
}

HttpFacesServer::HttpResponse HttpFacesServer::onCryptoRotate(const HttpRequest&) {
    if (!settings_) return jsonErr(503, "settings_unavailable", "settings 存储未初始化");
    const auto res = settings_->rotateKeyAndReencrypt();
    if (!res.ok) return jsonErr(res.httpStatus, res.code.empty() ? "internal_error" : res.code, res.message, res.details);
    return jsonOk(JsonValue::makeObject());
}

HttpFacesServer::HttpResponse HttpFacesServer::onFaces(const HttpRequest&) {
    if (!pipe_) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
    FacesSnapshot snap;
    if (!pipe_->snapshotFaces(snap)) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
    HttpResponse r;
    r.status = 200;
    r.reason = "OK";
    r.contentType = "application/json; charset=utf-8";
    r.body = buildFacesJson(snap);
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    r.headers.push_back({"Content-Security-Policy", "default-src 'none'"});
    return r;
}

HttpFacesServer::HttpResponse HttpFacesServer::onCameras(const HttpRequest&) {
    if (!pipe_) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
    const auto devs = pipe_->devices();
    std::ostringstream os;
    os << "{\"ok\":true,\"data\":{\"devices\":[";
    for (size_t i = 0; i < devs.size(); i++) {
        if (i) os << ",";
        os << "{"
           << "\"index\":" << i << ","
           << "\"name\":\"" << escapeJsonString(utf8FromWideLocal(devs[i].name)) << "\","
           << "\"deviceId\":\"" << escapeJsonString(utf8FromWideLocal(devs[i].deviceId)) << "\","
           << "\"formats\":[";
        for (size_t j = 0; j < devs[i].formats.size(); j++) {
            if (j) os << ",";
            const auto& f = devs[i].formats[j];
            os << "{\"w\":" << f.width << ",\"h\":" << f.height << ",\"fps\":" << f.fps << "}";
        }
        os << "]"
           << "}";
    }
    os << "]}}";
    return jsonOk(os.str());
}

HttpFacesServer::HttpResponse HttpFacesServer::onCameraFlip(const HttpRequest& req) {
    if (!pipe_) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
    JsonValue doc;
    std::string perr;
    if (!parseJson(req.body, doc, perr) || !doc.isObject()) return jsonErr(400, "invalid_request", "JSON 解析失败", {perr});
    bool flipX = false;
    bool flipY = false;
    if (const JsonValue* v = doc.find("flipX"); v && v->isBool()) flipX = v->b;
    if (const JsonValue* v = doc.find("flipY"); v && v->isBool()) flipY = v->b;
    pipe_->setFlip(flipX, flipY);
    return jsonOk(JsonValue::makeObject());
}

HttpFacesServer::HttpResponse HttpFacesServer::onEnroll(const HttpRequest& req) {
    if (!pipe_) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
    JsonValue doc;
    std::string perr;
    if (!parseJson(req.body, doc, perr) || !doc.isObject()) return jsonErr(400, "invalid_request", "JSON 解析失败", {perr});
    std::string personId;
    if (const JsonValue* v = doc.find("personId"); v && v->isString()) personId = v->s;
    personId = trim(personId);
    if (personId.empty()) return jsonErr(400, "invalid_request", "personId 不能为空");
    pipe_->requestEnroll(personId);
    return jsonOk(JsonValue::makeObject());
}

HttpFacesServer::HttpResponse HttpFacesServer::onDbClear(const HttpRequest&) {
    if (!pipe_) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
    pipe_->requestClearDb();
    return jsonOk(JsonValue::makeObject());
}

HttpFacesServer::HttpResponse HttpFacesServer::onPrivacyOpen(const HttpRequest&) {
    if (!pipe_) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
    pipe_->openPrivacySettings();
    return jsonOk(JsonValue::makeObject());
}

HttpFacesServer::HttpResponse HttpFacesServer::onPreviewJpg(const HttpRequest&) {
#if RK_WIN_HAS_OPENCV
    if (!pipe_) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
    RenderState rs;
    if (!pipe_->tryGetRenderState(rs)) return jsonErr(503, "frame_unavailable", "暂无可用画面");
    std::vector<std::uint8_t> jpeg;
    if (!buildJpegWithOverlay(rs, jpeg)) return jsonErr(500, "encode_failed", "JPEG 编码失败");
    HttpResponse r;
    r.status = 200;
    r.reason = "OK";
    r.contentType = "image/jpeg";
    r.headers.push_back({"Cache-Control", "no-cache"});
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    r.headers.push_back({"Content-Security-Policy", "default-src 'none'"});
    r.body.assign(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
    return r;
#else
    return jsonErr(503, "opencv_unavailable", "OpenCV 不可用，无法输出预览");
#endif
}

// ──── 流式会话 ───────────────────────────────────────────────

void HttpFacesServer::runStream(std::uintptr_t sock, std::unique_ptr<StreamSession> session) {
    const SOCKET s = static_cast<SOCKET>(sock);
    const std::string ct = session->contentType();
    std::ostringstream os;
    os << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: " << ct << "\r\n"
       << "Cache-Control: no-cache\r\n"
       << "X-Content-Type-Options: nosniff\r\n"
       << "X-Frame-Options: DENY\r\n"
       << "Content-Security-Policy: default-src 'none'\r\n"
       << "Connection: keep-alive\r\n"
       << "\r\n";
    const std::string head = os.str();
    if (!writeRaw(sock, head.data(), head.size())) {
        closesocket(s);
        return;
    }
    while (running_) {
        if (!session->writeFrame(sock, pipe_, running_)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(session->idleMs()));
    }
    closesocket(s);
}

// ──── 客户端处理 ─────────────────────────────────────────────

void HttpFacesServer::handleClient(std::uintptr_t sock) {
    const SOCKET s = static_cast<SOCKET>(sock);
    HttpRequest req;
    std::string err;

    if (!readRequest(sock, req, err)) {
        HttpResponse r;
        r.status = (err == "body_too_large") ? 413 : 400;
        r.reason = statusReason(r.status);
        r.contentType = "application/json; charset=utf-8";
        r.body = jsonErr(r.status, "invalid_request", "请求解析失败", {err}).body;
        (void)writeResponse(sock, r);
        closesocket(s);
        return;
    }

    // 流式端点：handleClient 中直接接管 socket 生命周期
    if ((req.path == "/api/faces/stream" || req.path == "/api/v1/faces/stream") && req.method == "GET") {
        runStream(sock, std::make_unique<SseSession>());
        return;
    }

    if (req.path == "/api/v1/preview.mjpeg" && req.method == "GET") {
        runStream(sock, std::make_unique<MjpegSession>());
        return;
    }

    const HttpResponse resp = handleRequest(req);
    (void)writeResponse(sock, resp);
    closesocket(s);
}

}  // namespace rk_win
