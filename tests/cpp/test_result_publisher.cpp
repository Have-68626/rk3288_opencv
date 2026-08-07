#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
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

    pub.publish(std::move(outcome));
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

    pub.publish(std::move(outcome));
    // 第二次，重建 outcome（同时间戳）→ 应被节流
    pipeline::FrameOutcome outcome2;
    pipeline::DomainEvent ev2;
    ev2.timestampMs = now;
    outcome2.events.push_back(ev2);
    pub.publish(std::move(outcome2));

    assert(count == 1);  // 只有第一次应触发回调
    return true;
}

bool test_publisher_concurrent_set_and_publish() {
    // 并发换回调与发布：writer 高频 setCallback 替换回调，reader 高频 publish 触发回调。
    // 修复前 callback_ 无锁，setCallback 的 move 赋值与 publish 读+调用并发 →
    // data race：读到半写回调而丢失（判空 false）或调用到已析构回调（UAF）。
    // 修复后 publish 锁内复制回调快照、锁外调用，每次触发发布必然调用一次完整回调。
    pipeline::ResultPublisher pub;
    auto calls = std::make_shared<std::atomic<long long>>(0);
    auto badMsgs = std::make_shared<std::atomic<long long>>(0);

    std::atomic<bool> stop{false};
    std::atomic<bool> callbackReady{false};
    std::atomic<long long> published{0};

    std::thread writer([&] {
        auto c = calls;
        auto b = badMsgs;
        pub.setCallback([c, b](const std::string& msg) {
            c->fetch_add(1);
            if (msg != "NO_FACE") b->fetch_add(1);
        });
        callbackReady.store(true);  // 首次回调就绪后再启动 reader，排除开头空回调窗口
        while (!stop.load()) {
            pub.setCallback([c, b](const std::string& msg) {
                c->fetch_add(1);
                if (msg != "NO_FACE") b->fetch_add(1);
            });
        }
    });

    std::thread reader([&] {
        while (!callbackReady.load()) std::this_thread::yield();
        long long ts = 100000;
        while (!stop.load()) {
            pipeline::FrameOutcome outcome;
            pipeline::DomainEvent ev;
            ev.type = "NO_FACE";
            ev.timestampMs = ts += 3000;  // 递增 3s 绕过 2s 节流窗口，确保每次触发回调
            outcome.events.push_back(ev);
            pub.publish(std::move(outcome));
            published.fetch_add(1);
        }
    });

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(600)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stop = true;
    writer.join();
    reader.join();

    // 修复后：每次触发发布恰好调用一次完整回调（锁内快照，无丢失/撕裂）
    return published.load() > 0 && calls->load() == published.load() && badMsgs->load() == 0;
}
