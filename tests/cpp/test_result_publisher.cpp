#include <cassert>
#include <string>
#include <chrono>
#include "pipeline/ResultPublisher.h"

bool test_publisher_emits_no_face_on_empty_tracks() {
    pipeline::ResultPublisher pub;
    std::string lastMsg;
    pub.setCallback([&](const std::string& msg) { lastMsg = msg; });

    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    pipeline::FrameOutcome outcome;
    pipeline::DomainEvent ev;
    ev.timestampMs = now;
    outcome.events.push_back(ev);
    // outcome.tracks is empty by default

    pub.publish(outcome);
    assert(lastMsg == "NO_FACE");
    return true;
}

bool test_publisher_throttles_repeated_calls() {
    pipeline::ResultPublisher pub;
    int count = 0;
    pub.setCallback([&](const std::string&) { count++; });

    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // 连续两次相同状态（无人脸）应在节流窗口内只触发一次
    pipeline::FrameOutcome outcome;
    pipeline::DomainEvent ev;
    ev.timestampMs = now;
    outcome.events.push_back(ev);

    pub.publish(outcome);
    pub.publish(outcome);  // 第二次，same timestamp → 应被节流

    assert(count == 1);  // 只有第一次应触发回调
    return true;
}
