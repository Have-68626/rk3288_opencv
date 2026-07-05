#pragma once

#include "Compat.h"
#include "ConnectionQuota.h"
#include "EndpointRegistry.h"
#include "JsonLite.h"
#include "StreamSessionRunner.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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

    // ──── 公共类型（端点注册表等外部组件可引用） ──────────────
public:
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

private:
    struct Route {
        const char* path;
        const char* method;
        HttpResponse (HttpFacesServer::*handler)(const HttpRequest&);
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
    static HttpResponse jsonOk(JsonValue data);
    static HttpResponse jsonErr(int httpStatus, const std::string& code, const std::string& message,
                                const std::vector<std::string>& details = {});

    // 端点 handler（由 Route 表调度）
    HttpResponse onHealth(const HttpRequest& req);
    HttpResponse onModels(const HttpRequest& req);
    HttpResponse onModelReload(const HttpRequest& req);
    HttpResponse onAcceleration(const HttpRequest& req);
    HttpResponse onOpenApi(const HttpRequest& req);
    HttpResponse onSettings(const HttpRequest& req);
    HttpResponse onCryptoRotate(const HttpRequest& req);
    HttpResponse onFaces(const HttpRequest& req);
    HttpResponse onCameras(const HttpRequest& req);
    HttpResponse onCameraFlip(const HttpRequest& req);
    HttpResponse onEnroll(const HttpRequest& req);
    HttpResponse onDbClear(const HttpRequest& req);
    HttpResponse onPrivacyOpen(const HttpRequest& req);
    HttpResponse onPreviewJpg(const HttpRequest& req);

    // 流式会话运行器
    StreamSessionRunner streamRunner_;

    // 端点注册表（替代 kRoutes + Route 表）
    std::unique_ptr<EndpointRegistry> registry_;

    std::atomic<bool> running_{false};
    FramePipeline* pipe_ = nullptr;
    EventLogger* events_ = nullptr;
    WinJsonConfigStore* settings_ = nullptr;
    int port_ = 0;

    std::thread acceptThread_;
    std::uintptr_t listenSock_ = 0;
    std::filesystem::path docRoot_;
    std::mutex clientMu_;

    static constexpr int MAX_CONCURRENT_CONNECTIONS = 64;
    ConnectionQuota quota_{MAX_CONCURRENT_CONNECTIONS};
    std::vector<std::uintptr_t> clientSocks_;
    std::atomic<int> activeClients_{0};
    std::mutex stopMu_;
    std::condition_variable stopCv_;

    // 客户端线程 RAII 管理（替代 detach）
    std::vector<std::thread> clientThreads_;
    std::mutex clientThreadsMu_;
    std::atomic<bool> serverStopping_{false};
};

}  // namespace rk_win

