#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace rk_win {

class EventLogger;
class FramePipeline;

class HttpFacesServer {
public:
    HttpFacesServer();
    ~HttpFacesServer();

    bool start(FramePipeline* pipe, EventLogger* events, int port);
    void stop();

private:
    void run();
    void handleClient(std::uintptr_t sock);

    std::atomic<bool> running_{false};
    FramePipeline* pipe_ = nullptr;
    EventLogger* events_ = nullptr;
    int port_ = 0;
    std::thread thread_;
    std::uintptr_t listenSock_ = 0;
};

}  // namespace rk_win

