#pragma once

#include <atomic>
#include "Compat.h"
#include <thread>

namespace rk_win {

class EventLogger;
class FramePipeline;

struct AbcTestConfig {
    bool testA = false;
    bool testB = false;
    bool testC = false;
    int repeatsB = 10;
    int secondsC = 30;
    std::filesystem::path reportDir = "docs/windows-camera-face-recognition";
    int httpPort = 8080;
};

class AbcTestRunner {
public:
    AbcTestRunner();
    ~AbcTestRunner();

    bool start(void* hwndMain, FramePipeline* pipe, EventLogger* events, const AbcTestConfig& cfg);
    void stop();

private:
    void run();

    std::atomic<bool> running_{false};
    void* hwndMain_ = nullptr;
    FramePipeline* pipe_ = nullptr;
    EventLogger* events_ = nullptr;
    AbcTestConfig cfg_{};
    std::thread thread_;
};

}  // namespace rk_win

