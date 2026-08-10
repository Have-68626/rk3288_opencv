#pragma once

#include "Compat.h"
#include "ConnectionQuota.h"
#include "JsonLite.h"
#include "StreamSessionRunner.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rk_win {

class EndpointRegistry;
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

    // ──── 有界客户端线程池（Q-CQ-B06-017）────────────────
    // 回收机制：每个客户端连接占一个 std::thread；线程体结束后置 done 标志，
    // 由 add() 之后的 reclaimFinished() 及时 join 并移除，避免已完成连接的
    // 线程句柄（OS thread handle）随请求总量无限累积。
    // 不变量：每次 reclaimFinished() 之后，池中仅存"仍在运行"的客户端线程，
    // 其数量 ≤ 并发连接上限（ConnectionQuota.max），故长期有界、峰值后回落。
    class ClientThreadPool {
    public:
        using DoneFlag = std::shared_ptr<std::atomic<bool>>;

        // 登记线程及其完成标志（线程体结束时应置 *done = true）。
        // done 先于线程体创建，供 acceptLoop 注入 lambda 捕获。
        void add(std::thread t, DoneFlag done) {
            std::lock_guard<std::mutex> lk(mu_);
            threads_.push_back({std::move(t), std::move(done)});
        }

        // 回收已完成线程（join 并移除），返回回收数量。
        // join 在锁外执行：持锁等待线程退出会阻塞 add()/size()，导致 acceptLoop 停滞。
        std::size_t reclaimFinished() {
            std::vector<std::thread> done;
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (auto it = threads_.begin(); it != threads_.end();) {
                    if (it->done->load() && it->th.joinable()) {
                        done.push_back(std::move(it->th));
                        it = threads_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (auto& t : done) t.join();
            return done.size();
        }

        std::size_t size() const {
            std::lock_guard<std::mutex> lk(mu_);
            return threads_.size();
        }

        // 停止：join 全部线程并清空。stop() 已先等待 activeClients_==0，
        // 此时所有线程体均已返回（done==true），join 不会阻塞。
        void joinAll() {
            std::vector<std::thread> all;
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& r : threads_) {
                    if (r.th.joinable()) all.push_back(std::move(r.th));
                }
                threads_.clear();
            }
            for (auto& t : all) t.join();
        }

    private:
        struct Record {
            std::thread th;
            DoneFlag done;
        };
        std::vector<Record> threads_;
        mutable std::mutex mu_;
    };

    // ──── 测试诊断接口（Q-CQ-B06-017）──────────────────
    // 当前跟踪的客户端线程数（运行中 + 已完成未回收）；等价于未回收线程句柄数。
    std::size_t clientThreadCount() const { return clientThreads_.size(); }
    // 当前活动客户端连接数（尚未完成请求处理的连接）。
    int activeClientCount() const { return activeClients_.load(); }

private:
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

    // 客户端线程池（替代裸 vector + 独立 mutex）：add 后立即回收已完成线程，
    // 保证线程/句柄长期有界、不随请求总量线性增长。
    ClientThreadPool clientThreads_;
    std::atomic<bool> serverStopping_{false};
};

}  // namespace rk_win

