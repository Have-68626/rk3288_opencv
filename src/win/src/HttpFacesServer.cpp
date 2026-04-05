#include "rk_win/HttpFacesServer.h"

#include "rk_win/EventLogger.h"
#include "rk_win/FacesJson.h"
#include "rk_win/FramePipeline.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <chrono>
#include <string>
#include <vector>

namespace rk_win {

HttpFacesServer::HttpFacesServer() = default;

HttpFacesServer::~HttpFacesServer() {
    stop();
}

bool HttpFacesServer::start(FramePipeline* pipe, EventLogger* events, int port) {
    stop();
    if (!pipe) return false;
    if (port <= 0 || port > 65535) return false;
    pipe_ = pipe;
    events_ = events;
    port_ = port;
    running_ = true;
    thread_ = std::thread(&HttpFacesServer::run, this);
    return true;
}

void HttpFacesServer::stop() {
    running_ = false;
#ifdef _WIN32
    if (listenSock_) {
        closesocket(static_cast<SOCKET>(listenSock_));
        listenSock_ = 0;
    }
#endif
    if (thread_.joinable()) thread_.join();
}

static bool sendAll(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int r = send(s, data + sent, static_cast<int>(len - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

static std::string readHttpRequest(SOCKET s) {
    std::string req;
    req.reserve(2048);
    char buf[1024];
    for (;;) {
        const int r = recv(s, buf, static_cast<int>(sizeof(buf)), 0);
        if (r <= 0) break;
        req.append(buf, buf + r);
        if (req.find("\r\n\r\n") != std::string::npos) break;
        if (req.size() > 8192) break;
    }
    return req;
}

static bool parseRequestLine(const std::string& req, std::string& method, std::string& path) {
    const size_t eol = req.find("\r\n");
    if (eol == std::string::npos) return false;
    const std::string line = req.substr(0, eol);
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    method = line.substr(0, sp1);
    path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    return true;
}

static void sendJsonResponse(SOCKET s, int code, const std::string& body) {
    const std::string status = (code == 200)   ? "200 OK"
                               : (code == 404) ? "404 Not Found"
                               : (code == 405) ? "405 Method Not Allowed"
                                               : "503 Service Unavailable";
    std::string hdr;
    hdr.reserve(256);
    hdr += "HTTP/1.1 " + status + "\r\n";
    hdr += "Content-Type: application/json; charset=utf-8\r\n";
    hdr += "Cache-Control: no-cache\r\n";
    hdr += "X-Content-Type-Options: nosniff\r\n";
    hdr += "X-Frame-Options: DENY\r\n";
    hdr += "Content-Security-Policy: default-src 'none'\r\n";
    hdr += "Connection: close\r\n";
    hdr += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    hdr += "\r\n";
    sendAll(s, hdr.c_str(), hdr.size());
    sendAll(s, body.c_str(), body.size());
}

void HttpFacesServer::handleClient(std::uintptr_t sock0) {
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(sock0);
    DWORD recvTimeoutMs = 2000;
    DWORD sendTimeoutMs = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTimeoutMs), sizeof(recvTimeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeoutMs), sizeof(sendTimeoutMs));

    const std::string req = readHttpRequest(s);
    std::string method;
    std::string path;
    if (!parseRequestLine(req, method, path)) {
        sendJsonResponse(s, 404, "{}");
        closesocket(s);
        return;
    }
    if (method != "GET") {
        sendJsonResponse(s, 405, "{}");
        closesocket(s);
        return;
    }

    if (path == "/api/faces") {
        FacesSnapshot snap;
        if (!pipe_ || !pipe_->snapshotFaces(snap)) {
            sendJsonResponse(s, 503, "{}");
            closesocket(s);
            return;
        }
        const std::string body = buildFacesJson(snap);
        sendJsonResponse(s, 200, body);
        closesocket(s);
        return;
    }

    if (path == "/api/faces/stream") {
        std::string hdr;
        hdr += "HTTP/1.1 200 OK\r\n";
        hdr += "Content-Type: text/event-stream; charset=utf-8\r\n";
        hdr += "Cache-Control: no-cache\r\n";
        hdr += "X-Content-Type-Options: nosniff\r\n";
        hdr += "X-Frame-Options: DENY\r\n";
        hdr += "Content-Security-Policy: default-src 'none'\r\n";
        hdr += "Connection: keep-alive\r\n";
        hdr += "\r\n";
        if (!sendAll(s, hdr.c_str(), hdr.size())) {
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
                    if (!sendAll(s, evt.c_str(), evt.size())) break;
                }
            } else {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastKeep > std::chrono::seconds(10)) {
                    const std::string ka = ": keepalive\n\n";
                    if (!sendAll(s, ka.c_str(), ka.size())) break;
                    lastKeep = now;
                }
            }
        }
        closesocket(s);
        return;
    }

    sendJsonResponse(s, 404, "{}");
    closesocket(s);
#else
    (void)sock0;
#endif
}

void HttpFacesServer::run() {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        WSACleanup();
        return;
    }
    listenSock_ = static_cast<std::uintptr_t>(ls);

    BOOL reuse = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port_));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(ls);
        listenSock_ = 0;
        WSACleanup();
        return;
    }

    if (listen(ls, SOMAXCONN) != 0) {
        closesocket(ls);
        listenSock_ = 0;
        WSACleanup();
        return;
    }

    if (events_) events_->append("http_server_start", "listen=127.0.0.1:" + std::to_string(port_));

    while (running_) {
        sockaddr_in caddr{};
        int clen = sizeof(caddr);
        SOCKET cs = accept(ls, reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (cs == INVALID_SOCKET) {
            if (!running_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::thread(&HttpFacesServer::handleClient, this, static_cast<std::uintptr_t>(cs)).detach();
    }

    closesocket(ls);
    listenSock_ = 0;
    if (events_) events_->append("http_server_stop", "listen=127.0.0.1:" + std::to_string(port_));
    WSACleanup();
#endif
}

}  // namespace rk_win

