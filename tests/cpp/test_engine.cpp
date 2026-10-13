#include <gtest/gtest.h>

// flip 应用与检测框绘制提取为 Engine 公共静态纯函数（applyFlip/drawTracks），
// 本测试直接验证其不变量，无需 private 访问。
#include "Engine.h"
#include "Config.h"

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

// 镜像翻转不原地修改输入帧（RK3288 高频来回翻转回归）。
// 修复前：preprocessFrame 尺寸匹配时 processedFrame_=frame 浅拷贝后原地
// cv::flip(processedFrame_,processedFrame_,code) 修改共享缓冲 → 输入帧被翻转（RED）；
// 修复后：applyFlip 翻到新 Mat，输入帧保持原样，发布缓冲稳定不再按拍频抖动。
TEST(EngineTest, ApplyFlipDoesNotModifyInput) {
    // 非对称图案：左半边蓝、右半边红——原地翻转必然改变像素，可检出回归
    cv::Mat input(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(0, 0, 255));
    input(cv::Rect(0, 0, rk_core::config::FRAME_WIDTH / 2, rk_core::config::FRAME_HEIGHT))
        .setTo(cv::Scalar(255, 0, 0));
    cv::Mat before = input.clone();
    cv::Mat out = rk_core::Engine::applyFlip(input, true, false);  // 水平翻转
    // 输入帧必须未被原地翻转（reshape(1) 展平多通道供 countNonZero 计数）
    const cv::Mat diffMask = before != input;
    EXPECT_EQ(cv::countNonZero(diffMask.reshape(1)), 0);
    // 输出应为水平镜像：左上像素 == 输入右上像素
    ASSERT_EQ(out.cols, rk_core::config::FRAME_WIDTH);
    const cv::Vec3b left = out.at<cv::Vec3b>(0, 0);
    const cv::Vec3b right = input.at<cv::Vec3b>(0, rk_core::config::FRAME_WIDTH - 1);
    EXPECT_EQ(left[0], right[0]);
    EXPECT_EQ(left[1], right[1]);
    EXPECT_EQ(left[2], right[2]);
}

// 渲染帧须含人脸检测框（三层重构 029e43f 删除 drawTracks 后无框 → RED）。
// drawTracks 按 tracks 在帧上画框：认证绿框/未认证红框。
TEST(EngineTest, DrawTracksDrawsFaceBoxes) {
    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(10, 10, 10));  // 深色底
    pipeline::TrackView track;
    track.trackId = 1;
    track.stableId = "alice";
    track.stableConfidence = 0.95f;
    track.bbox = cv::Rect(100, 80, 200, 240);
    std::vector<pipeline::TrackView> tracks = {track};
    rk_core::Engine::drawTracks(frame, tracks);
    // 检测框顶边（y=80）应被绘制（绿框 G 通道 != 背景 10）
    const cv::Vec3b border = frame.at<cv::Vec3b>(80, 150);
    EXPECT_NE(border[1], 10);
    // 框外像素保持背景
    const cv::Vec3b outside = frame.at<cv::Vec3b>(10, 10);
    EXPECT_EQ(outside[0], 10);
    EXPECT_EQ(outside[1], 10);
    EXPECT_EQ(outside[2], 10);
}
