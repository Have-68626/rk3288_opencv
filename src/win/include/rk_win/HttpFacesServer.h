#pragma once

#include "Compat.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rk_win {

class EventLogger;
class FramePipeline;
class WinJsonConfigStore;

class HttpFacesServer {
public:
    HttpFacesServer();
    ~HttpFacesServer();

    bool start(FramePipeline* pipe, EventLogger* events, int port, WinJsonConfigStore* settings);
    void stop();

private:
    struct HttpRequest {
        std::string method;
        std::string path;
        std::string query;
        std::string httpVersion;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
    };

    struct HttpResponse {
        int status = 200;
        std::string reason = "OK";
        std::string contentType = "application/json; charset=utf-8";
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        bool close = true;
    };

    void acceptLoop();
    void handleClient(std::uintptr_t sock);
    bool readRequest(std::uintptr_t sock, HttpRequest& out, std::string& err);
    bool writeResponse(std::uintptr_t sock, const HttpResponse& resp);
    bool writeRaw(std::uintptr_t sock, const void* data, std::size_t n);

    HttpResponse handleRequest(const HttpRequest& req);
    HttpResponse handleApi(const HttpRequest& req);
    HttpResponse handleStaticOrFallback(const HttpRequest& req);

    static HttpResponse jsonOk(const std::string& bodyJson);
    static HttpResponse jsonErr(int httpStatus, const std::string& code, const std::string& message,
                                const std::vector<std::string>& details = {});

    std::atomic<bool> running_{false};
    FramePipeline* pipe_ = nullptr;
    EventLogger* events_ = nullptr;
    WinJsonConfigStore* settings_ = nullptr;
    int port_ = 0;

    std::thread acceptThread_;
    std::uintptr_t listenSock_ = 0;
    std::filesystem::path docRoot_;
    std::mutex clientMu_;
};

}  // namespace rk_win

