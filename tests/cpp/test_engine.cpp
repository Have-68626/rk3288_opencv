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

// 奇数宽/高 NV21 帧：OpenCV cvtColor 断言失败（sz.width%2==0 && sz.height%3==0）抛异常，
// 修复前异常逃出 run() 循环导致进程崩溃；修复后 toBgrFromNv21 显式拒绝，循环存活
TEST(EngineTest, OddNv21FrameRejected) {
    Engine engine;
    engine.setExternalInputEnabled(true);
    std::thread runner([&engine] { engine.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ExternalFrame odd;
    odd.format = ExternalFrameFormat::NV21;
    odd.width = 3;
    odd.height = 5;
    odd.nv21RowStrideY = 3;
    odd.nv21.assign(static_cast<std::size_t>(3 * 5 + 3 * 2), 128);
    engine.pushExternalFrame(odd);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 偶数 NV21：循环应仍存活并处理
    ExternalFrame even;
    even.format = ExternalFrameFormat::NV21;
    even.width = 4;
    even.height = 6;
    even.nv21RowStrideY = 4;
    even.nv21.assign(static_cast<std::size_t>(4 * 6 + 4 * 3), 128);
    engine.pushExternalFrame(even);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    engine.stop();
    runner.join();
    // 到达此处即证明：奇数帧未崩溃进程、未摧毁 run 循环
    SUCCEED();
}
