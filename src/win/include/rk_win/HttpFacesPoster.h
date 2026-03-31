#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace rk_win {

class EventLogger;
class FramePipeline;

class HttpFacesPoster {
public:
    HttpFacesPoster();
    ~HttpFacesPoster();

    bool start(FramePipeline* pipe, EventLogger* events, const std::string& postUrl, int throttleMs, int backoffMinMs, int backoffMaxMs);
    void stop();

private:
    void run();

    std::atomic<bool> running_{false};
    FramePipeline* pipe_ = nullptr;
    EventLogger* events_ = nullptr;
    std::string postUrl_;
    int throttleMs_ = 100;
    int backoffMinMs_ = 200;
    int backoffMaxMs_ = 5000;
    std::thread thread_;
};

}  // namespace rk_win

