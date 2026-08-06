#include <gtest/gtest.h>
#include "Engine.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace rk_core;

TEST(EngineTest, CreateAndDestroy) {
    Engine engine;
    // 验证 engine 可正常构造和析构
    SUCCEED();
}

TEST(EngineTest, InitializeWithEmptyPath) {
    Engine engine;
    // 空路径应返回 false 而非崩溃
    EXPECT_FALSE(engine.initialize("", "", ""));
}

// 首次失败后状态不得被污染：坏 cascade 初始化失败后，
// 再次初始化必须重新执行 initCommon（仍失败），而非"跳过重复调用"假成功
TEST(EngineTest, FailedInitDoesNotPolluteState) {
    Engine engine;
    EXPECT_FALSE(engine.initialize(-1, "bad_cascade_path.xml", ""));
    EXPECT_FALSE(engine.initialize(-1, "bad_cascade_path.xml", ""));
}

// 并发初始化失败：所有线程都必须失败（坏配置），不污染状态、不崩溃
TEST(EngineTest, ConcurrentInitFailure) {
    Engine engine;
    std::atomic<int> okCount{0};
    constexpr int kThreads = 8;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int i = 0; i < kThreads; i++) {
        ts.emplace_back([&engine, &okCount] {
            if (engine.initialize(-1, "bad_cascade_path.xml", "")) {
                okCount.fetch_add(1);
            }
        });
    }
    for (auto& t : ts) t.join();
    // 坏配置下所有并发初始化都必须失败
    EXPECT_EQ(okCount.load(), 0);
}
