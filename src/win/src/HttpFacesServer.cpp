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
static bool buildJpegWithOverlay(const RenderState& rs, std::vector<std::uint8_t>& outJpeg) {
    if (rs.bgr.empty()) return false;
    cv::Mat img = rs.bgr.clone();
    for (const auto& f : rs.faces) {
        cv::rectangle(img, f.rect, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
    }
    std::vector<int> params;
    params.push_back(cv::IMWRITE_JPEG_QUALITY);
    params.push_back(80);
    std::vector<uchar> buf;
    if (!cv::imencode(".jpg", img, buf, params)) return false;
    outJpeg.assign(buf.begin(), buf.end());
    return true;
}
#endif

}  // namespace

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
}

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

        std::thread([this, cs]() { handleClient(static_cast<std::uintptr_t>(cs)); }).detach();
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

HttpFacesServer::HttpResponse HttpFacesServer::handleRequest(const HttpRequest& req) {
    if (startsWith(req.path, "/api")) return handleApi(req);
    return handleStaticOrFallback(req);
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
    return r;
}

HttpFacesServer::HttpResponse HttpFacesServer::handleApi(const HttpRequest& req) {
    const std::string& m = req.method;
    const std::string& p = req.path;

    if (p == "/api/v1/health") {
        if (m != "GET") return jsonErr(405, "method_not_allowed", "仅支持 GET");
        JsonValue o = JsonValue::makeObject();
        o.o["ok"] = JsonValue::makeBool(true);
        JsonValue d = JsonValue::makeObject();
        d.o["name"] = JsonValue::makeString("rk_wcfr");
        d.o["port"] = JsonValue::makeNumber(port_);
        o.o["data"] = std::move(d);
        return jsonOk(toJsonString(o, false));
    }

    if (p == "/api/v1/models") {
        if (m != "GET") return jsonErr(405, "method_not_allowed", "仅支持 GET");
        JsonValue o = JsonValue::makeObject();
        o.o["ok"] = JsonValue::makeBool(true);

        JsonValue d = JsonValue::makeObject();

        JsonValue supported = JsonValue::makeArray();
        {
            JsonValue s1 = JsonValue::makeObject();
            s1.o["id"] = JsonValue::makeString("cascade_frontalface");
            s1.o["displayName"] = JsonValue::makeString("Cascade Frontal Face (LBP)");
            s1.o["taskType"] = JsonValue::makeString("detect_recognize_pipeline");
            supported.a.push_back(std::move(s1));

            JsonValue s2 = JsonValue::makeObject();
            s2.o["id"] = JsonValue::makeString("dnn_face_detector");
            s2.o["displayName"] = JsonValue::makeString("OpenCV DNN Face Detector");
            s2.o["taskType"] = JsonValue::makeString("detect");
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

        o.o["data"] = std::move(d);
        return jsonOk(toJsonString(o, false));
    }

    if (p == "/api/v1/openapi" || p == "/openapi.json") {
        if (m != "GET") return jsonErr(405, "method_not_allowed", "仅支持 GET");
        JsonValue o = JsonValue::makeObject();
        o.o["ok"] = JsonValue::makeBool(true);
        JsonValue d = JsonValue::makeObject();
        d.o["note"] = JsonValue::makeString("OpenAPI 文档见 docs/windows-web-spa/openapi.yaml；此端点为最小联调占位");
        o.o["data"] = std::move(d);
        return jsonOk(toJsonString(o, false));
    }

    if (p == "/api/v1/settings") {
        if (!settings_) return jsonErr(503, "settings_unavailable", "settings 存储未初始化");
        if (m == "GET") {
            const std::string redacted = settings_->currentRedactedJsonPretty();
            std::ostringstream os;
            os << "{\"ok\":true,\"data\":" << redacted << "}";
            return jsonOk(os.str());
        }
        if (m == "PUT") {
            const auto res = settings_->updateFromJsonBody(req.body);
            if (!res.ok) return jsonErr(res.httpStatus, res.code.empty() ? "invalid_request" : res.code, res.message, res.details);
            const std::string redacted = settings_->currentRedactedJsonPretty();
            std::ostringstream os;
            os << "{\"ok\":true,\"data\":" << redacted << "}";
            return jsonOk(os.str());
        }
        return jsonErr(405, "method_not_allowed", "仅支持 GET/PUT");
    }

    if (p == "/api/v1/actions/crypto/rotate") {
        if (!settings_) return jsonErr(503, "settings_unavailable", "settings 存储未初始化");
        if (m != "POST") return jsonErr(405, "method_not_allowed", "仅支持 POST");
        const auto res = settings_->rotateKeyAndReencrypt();
        if (!res.ok) return jsonErr(res.httpStatus, res.code.empty() ? "internal_error" : res.code, res.message, res.details);
        JsonValue o = JsonValue::makeObject();
        o.o["ok"] = JsonValue::makeBool(true);
        return jsonOk(toJsonString(o, false));
    }

    if (p == "/api/faces" || p == "/api/v1/faces") {
        if (m != "GET") return jsonErr(405, "method_not_allowed", "仅支持 GET");
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

    if (p == "/api/faces/stream" || p == "/api/v1/faces/stream") {
        if (m != "GET") return jsonErr(405, "method_not_allowed", "仅支持 GET");
        HttpResponse r;
        r.status = 200;
        r.reason = "OK";
        r.contentType = "text/event-stream; charset=utf-8";
        r.close = false;
        r.headers.push_back({"Cache-Control", "no-cache"});
        r.headers.push_back({"Connection", "keep-alive"});
        r.headers.push_back({"X-Content-Type-Options", "nosniff"});
        r.headers.push_back({"X-Frame-Options", "DENY"});
        r.headers.push_back({"Content-Security-Policy", "default-src 'none'"});
        r.body.clear();
        return r;
    }

    if (p == "/api/v1/cameras") {
        if (m != "GET") return jsonErr(405, "method_not_allowed", "仅支持 GET");
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

    if (p == "/api/v1/camera/flip") {
        if (m != "PUT") return jsonErr(405, "method_not_allowed", "仅支持 PUT");
        if (!pipe_) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
        JsonValue doc;
        std::string perr;
        if (!parseJson(req.body, doc, perr) || !doc.isObject()) return jsonErr(400, "invalid_request", "JSON 解析失败", {perr});
        bool flipX = false;
        bool flipY = false;
        if (const JsonValue* v = doc.find("flipX"); v && v->isBool()) flipX = v->b;
        if (const JsonValue* v = doc.find("flipY"); v && v->isBool()) flipY = v->b;
        pipe_->setFlip(flipX, flipY);
        JsonValue o = JsonValue::makeObject();
        o.o["ok"] = JsonValue::makeBool(true);
        return jsonOk(toJsonString(o, false));
    }

    if (p == "/api/v1/actions/enroll") {
        if (m != "POST") return jsonErr(405, "method_not_allowed", "仅支持 POST");
        if (!pipe_) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
        JsonValue doc;
        std::string perr;
        if (!parseJson(req.body, doc, perr) || !doc.isObject()) return jsonErr(400, "invalid_request", "JSON 解析失败", {perr});
        std::string personId;
        if (const JsonValue* v = doc.find("personId"); v && v->isString()) personId = v->s;
        personId = trim(personId);
        if (personId.empty()) return jsonErr(400, "invalid_request", "personId 不能为空");
        pipe_->requestEnroll(personId);
        JsonValue o = JsonValue::makeObject();
        o.o["ok"] = JsonValue::makeBool(true);
        return jsonOk(toJsonString(o, false));
    }

    if (p == "/api/v1/actions/db/clear") {
        if (m != "POST") return jsonErr(405, "method_not_allowed", "仅支持 POST");
        if (!pipe_) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
        pipe_->requestClearDb();
        JsonValue o = JsonValue::makeObject();
        o.o["ok"] = JsonValue::makeBool(true);
        return jsonOk(toJsonString(o, false));
    }

    if (p == "/api/v1/actions/privacy/open") {
        if (m != "POST") return jsonErr(405, "method_not_allowed", "仅支持 POST");
        if (!pipe_) return jsonErr(503, "pipeline_unavailable", "Pipeline 不可用");
        pipe_->openPrivacySettings();
        JsonValue o = JsonValue::makeObject();
        o.o["ok"] = JsonValue::makeBool(true);
        return jsonOk(toJsonString(o, false));
    }

    if (p == "/api/v1/preview.jpg") {
        if (m != "GET") return jsonErr(405, "method_not_allowed", "仅支持 GET");
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

    if (p == "/api/v1/preview.mjpeg") {
        if (m != "GET") return jsonErr(405, "method_not_allowed", "仅支持 GET");
        HttpResponse r;
        r.status = 200;
        r.reason = "OK";
        r.contentType = "multipart/x-mixed-replace; boundary=rk_boundary";
        r.close = false;
        r.headers.push_back({"Cache-Control", "no-cache"});
        r.headers.push_back({"X-Content-Type-Options", "nosniff"});
        r.headers.push_back({"X-Frame-Options", "DENY"});
        r.headers.push_back({"Content-Security-Policy", "default-src 'none'"});
        r.headers.push_back({"Connection", "close"});
        r.body.clear();
        return r;
    }

    return jsonErr(404, "not_found", "未知端点");
}

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

    if ((req.path == "/api/faces/stream" || req.path == "/api/v1/faces/stream") && req.method == "GET") {
        const std::string head =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "Cache-Control: no-cache\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "X-Frame-Options: DENY\r\n"
            "Content-Security-Policy: default-src 'none'\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        if (!writeRaw(sock, head.data(), head.size())) {
            closesocket(s);
            return;
        }
        std::uint64_t lastSeq = 0;
        auto lastKeep = std::chrono::steady_clock::now();
        while (running_) {
            std::uint64_t seq = 0;
            const bool changed = pipe_ ? pipe_->waitFacesSeqChanged(lastSeq, 1000, seq) : false;
            if (!running_) break;
            if (changed) {
                lastSeq = seq;
                FacesSnapshot snap;
                if (pipe_ && pipe_->snapshotFaces(snap)) {
                    const std::string body = buildFacesJson(snap);
                    std::string evt;
                    evt.reserve(body.size() + 32);
                    evt += "data: ";
                    evt += body;
                    evt += "\n\n";
                    if (!writeRaw(sock, evt.data(), evt.size())) break;
                }
            } else {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastKeep > std::chrono::seconds(10)) {
                    const std::string ka = ": keepalive\n\n";
                    if (!writeRaw(sock, ka.data(), ka.size())) break;
                    lastKeep = now;
                }
            }
        }
        closesocket(s);
        return;
    }

    if (req.path == "/api/v1/preview.mjpeg" && req.method == "GET") {
        const std::string boundary = "rk_boundary";
        std::ostringstream os;
        os << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: multipart/x-mixed-replace; boundary=" << boundary << "\r\n"
           << "Cache-Control: no-cache\r\n"
           << "X-Content-Type-Options: nosniff\r\n"
           << "X-Frame-Options: DENY\r\n"
           << "Content-Security-Policy: default-src 'none'\r\n"
           << "Connection: close\r\n"
           << "\r\n";
        const std::string head = os.str();
        if (!writeRaw(sock, head.data(), head.size())) {
            closesocket(s);
            return;
        }
#if RK_WIN_HAS_OPENCV
        while (running_) {
            if (!pipe_) break;
            RenderState rs;
            if (!pipe_->tryGetRenderState(rs)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::vector<std::uint8_t> jpeg;
            if (!buildJpegWithOverlay(rs, jpeg)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::ostringstream part;
            part << "--" << boundary << "\r\n"
                 << "Content-Type: image/jpeg\r\n"
                 << "Content-Length: " << jpeg.size() << "\r\n"
                 << "\r\n";
            const std::string partHead = part.str();
            if (!writeRaw(sock, partHead.data(), partHead.size())) break;
            if (!writeRaw(sock, jpeg.data(), jpeg.size())) break;
            if (!writeRaw(sock, "\r\n", 2)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
#endif
        closesocket(s);
        return;
    }

    const HttpResponse resp = handleRequest(req);
    (void)writeResponse(sock, resp);
    closesocket(s);
}

}  // namespace rk_win

