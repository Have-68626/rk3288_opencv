#include <gtest/gtest.h>

// flip 应用与检测框绘制提取为 Engine 公共静态纯函数（applyFlip/drawTracks），
// 本测试直接验证其不变量，无需 private 访问。
#include "Engine.h"
#include "Config.h"
#include "FaceSearch.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace rk_core;

namespace {

// 确定性交错测试的统一等待上限（第四轮审查 H1）：所有跨线程等待（embed 阻塞点、测试侧等待
// 引擎入场、发布屏障）都必须有界。若引擎路径变更导致等待点未被触达（例如快照非 effective、
// assignRealtimeIdentities 在 !gallery 处提前返回、或帧线程先抛异常），无界等待会让测试
// **永久阻塞**，CI 表现为 job 超时而非干净失败。超时后各等待点放行并以断言失败收尾，
// 保证「快速、干净的失败」。
constexpr auto kInterleaveWait = std::chrono::seconds(5);

// ── 任务12 实时路径测试替身 ──

// 与 test_realtime_face_pipeline.cpp 同构的合法 manifest v1（9 输出 + 512 维）。
std::string rtManifest() {
    return
        "{\"schemaVersion\":1,"
        "\"profileId\":\"rk3288_light\",\"displayName\":\"RK3288 Light\","
        "\"supportedAbis\":[\"armeabi-v7a\",\"arm64-v8a\"],"
        "\"detector\":{\"param\":{\"path\":\"scrfd/scrfd.param\"},"
        "\"bin\":{\"path\":\"scrfd/scrfd.bin\"},"
        " \"input\":\"in0\","
        " \"outputs\":[\"out0\",\"out1\",\"out2\",\"out3\",\"out4\",\"out5\",\"out6\",\"out7\",\"out8\"],"
        " \"inputWidth\":640,\"inputHeight\":640},"
        "\"embedder\":{\"param\":{\"path\":\"arc/arc.param\"},"
        "\"bin\":{\"path\":\"arc/arc.bin\"},"
        " \"input\":\"in0\",\"output\":\"out0\",\"inputWidth\":112,\"inputHeight\":112,"
        " \"embeddingDim\":512},"
        "\"detectionConfidence\":0.55,\"nmsIou\":0.40,\"identitySimilarity\":0.45,"
        "\"modelVersion\":\"1\",\"licenseId\":\"apache-2.0\"}";
}

// 可脚本化检测器：boxes 即本次返回的检测框（空 = 实际空检测）。
class ScriptedDetector : public IRealtimeDetector {
public:
    std::vector<RealtimeFaceBox> boxes;
    bool detect(const cv::Mat&, std::vector<RealtimeFaceBox>& out, std::string&) override {
        out = boxes;
        return true;
    }
    const char* name() const override { return "scripted"; }
};

// 内容感知检测器：扫描画面中的白色标记并返回其外接框（仅依赖 cv::Mat，无 imgproc）。
// 用于验证「变换先于检测」——标记随统一变换移动，检测框随之落在变换后坐标。
class EdgeMarkerDetector : public IRealtimeDetector {
public:
    bool detect(const cv::Mat& frame, std::vector<RealtimeFaceBox>& out, std::string&) override {
        int minX = frame.cols, minY = frame.rows, maxX = -1, maxY = -1;
        for (int y = 0; y < frame.rows; ++y) {
            const cv::Vec3b* row = frame.ptr<cv::Vec3b>(y);
            for (int x = 0; x < frame.cols; ++x) {
                const cv::Vec3b& p = row[x];
                if (p[0] > 200 && p[1] > 200 && p[2] > 200) {
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                }
            }
        }
        if (maxX < 0) return true;  // 无标记 = 空检测
        out.push_back(RealtimeFaceBox{static_cast<float>(minX), static_cast<float>(minY),
                                      static_cast<float>(maxX - minX + 1),
                                      static_cast<float>(maxY - minY + 1), 0.9f});
        return true;
    }
    const char* name() const override { return "edge_marker"; }
};

// 常量嵌入：全 1 向量；与全 1 图库条目点积 = dim，便于构造确定性命中。
class ConstantEmbedder : public IRealtimeEmbedder {
public:
    bool embed(const cv::Mat&, std::vector<float>& out, std::string&) override {
        out.assign(static_cast<std::size_t>(512), 1.0f);
        return true;
    }
    int embeddingDim() const override { return 512; }
    const char* name() const override { return "const"; }
};

// 内容感知嵌入：对对齐图按固定步长采样成 512 维向量，并记录每次收到的对齐输入（clone）。
// 同一帧 + 同检测走同一对齐时向量逐元素相同；bbox 与关键点两种对齐则显著不同。
// 仅依赖 cv::Mat 数据指针（不引入 imgproc），使「对齐一致性」可观测。
class ContentRecordingEmbedder : public IRealtimeEmbedder {
public:
    std::mutex mu;
    std::vector<cv::Mat> alignedInputs;

    bool embed(const cv::Mat& in, std::vector<float>& out, std::string&) override {
        if (in.empty() || in.cols <= 0 || in.rows <= 0) return false;
        {
            std::lock_guard<std::mutex> lock(mu);
            alignedInputs.push_back(in.clone());
        }
        const int total = in.rows * in.cols;
        std::vector<float> v(static_cast<std::size_t>(512), 0.0f);
        const std::size_t esz = in.elemSize();
        for (int i = 0; i < 512; ++i) {
            const int idx = (i * total) / 512;
            const int y = idx / in.cols;
            const int x = idx % in.cols;
            const unsigned char* p = in.ptr<unsigned char>(y) + static_cast<std::size_t>(x) * esz;
            v[static_cast<std::size_t>(i)] = static_cast<float>(p[0]);
        }
        out = std::move(v);
        return true;
    }
    int embeddingDim() const override { return 512; }
    const char* name() const override { return "content_rec"; }
};

// 双色调嵌入：按对齐脸灰度均值二值化为两种可区分的常量向量，用于构造「两张不同身份
// 的人脸」场景（ConstantEmbedder 对两张脸给同一向量，无法区分 alice/bob）。亮区 → 全 1
// （匹配 alice 模板）；暗区 → 前 256 个 1（匹配 bob 模板）。
class TwoToneEmbedder : public IRealtimeEmbedder {
public:
    bool embed(const cv::Mat& in, std::vector<float>& out, std::string&) override {
        if (in.empty() || in.cols <= 0 || in.rows <= 0) return false;
        long long sum = 0;
        const std::size_t esz = in.elemSize();
        for (int y = 0; y < in.rows; ++y) {
            const unsigned char* row = in.ptr<unsigned char>(y);
            for (int x = 0; x < in.cols; ++x) {
                sum += row[static_cast<std::size_t>(x) * esz];
            }
        }
        const double mean =
            static_cast<double>(sum) / (static_cast<double>(in.rows) * in.cols);
        out.assign(512, 0.0f);
        if (mean >= 128.0) {
            for (float& v : out) v = 1.0f;  // 亮区 → alice 模板
        } else {
            for (int i = 0; i < 256; ++i) out[static_cast<std::size_t>(i)] = 1.0f;  // 暗区 → bob 模板
        }
        return true;
    }
    int embeddingDim() const override { return 512; }
    const char* name() const override { return "two_tone"; }
};

// 阻塞式嵌入：embed() 内以 condition_variable 等待测试线程放行，用于把引擎线程**确定性钉在**
// 「assignRealtimeIdentities 记录 frameGalleryGeneration_ 之后、trackFaces 提交之前」的窗口
//（embed 恰在此窗口内被调用），从而可靠复现「读库 → 提交」之间换库的交错（替代 sleep 竞态）。
// 放行后 embed 仍返回全 1 向量（与 ConstantEmbedder 一致），使旧库检索确定性命中。
// 第四轮审查 H1：embed 内等待**有界**（kInterleaveWait）。若引擎路径变更导致测试未在合理时间
// 放行（例如前置断言失败提前返回），等待超时即置 timedOut 并继续返回，绝不永久阻塞引擎线程。
class BlockingEmbedder : public IRealtimeEmbedder {
public:
    std::mutex mu;
    std::condition_variable cv;
    bool blockNext = false;  // 置 true → 下一次 embed() 阻塞在窗口内
    bool entered = false;    // 已进入 embed 并阻塞（测试线程据此判定已钉住）
    bool released = false;   // 测试线程置 true 放行
    bool timedOut = false;   // 诊断：等待放行超时（>kInterleaveWait）。仅供排查；测试侧以自身有界
                             // 等待 + ASSERT 判定失败，故不对本标记断言（避免与放行时机竞态）

    bool embed(const cv::Mat&, std::vector<float>& out, std::string&) override {
        {
            std::unique_lock<std::mutex> lock(mu);
            if (blockNext) {
                blockNext = false;
                entered = true;
                cv.notify_all();
                // 有界等待：测试线程未及时放行时超时返回（置 timedOut），保证快速干净失败。
                if (!cv.wait_for(lock, kInterleaveWait, [this] { return released; })) {
                    timedOut = true;
                }
                released = false;
                entered = false;
            }
        }
        out.assign(static_cast<std::size_t>(512), 1.0f);
        return true;
    }
    int embeddingDim() const override { return 512; }
    const char* name() const override { return "blocking"; }
};

// 构造注入用实时管线；reload=false 时故意不发布快照（模拟模型未加载）。
// embedder 非空时用其替换默认 ConstantEmbedder，供对齐一致性用例复用同一实例。
std::shared_ptr<RealtimeFacePipeline> makeRtPipeline(
    std::shared_ptr<IRealtimeDetector> det,
    std::shared_ptr<const FaceSearchLinearIndex> gallery = nullptr,
    bool reload = true,
    std::shared_ptr<IRealtimeEmbedder> embedder = nullptr) {
    RealtimePipelineHooks h;
    h.verifyFiles = [](const FaceRuntimeProfile&, const std::string&, std::string&) { return true; };
    h.loadDetector = [det](const FaceRuntimeProfile&, const std::string&, std::string&)
        -> std::shared_ptr<IRealtimeDetector> { return det; };
    h.loadEmbedder = [embedder](const FaceRuntimeProfile&, const std::string&, std::string&)
        -> std::shared_ptr<IRealtimeEmbedder> {
        return embedder ? embedder : std::make_shared<ConstantEmbedder>();
    };
    h.loadGallery = [gallery](const FaceRuntimeProfile&, const std::string&, std::string&)
        -> std::shared_ptr<const FaceSearchLinearIndex> { return gallery; };
    h.selfCheck = [](const RealtimePipelineSnapshot&, std::string&) { return true; };
    h.benchmark = [](const RealtimePipelineSnapshot&, const RealtimeBenchmarkConfig&,
                     double& p95, std::string&) { p95 = 1.0; return true; };
    auto pipe = std::make_shared<RealtimeFacePipeline>(h);
    if (reload) {
        pipe->reload(rtManifest(), "", "arm64-v8a", "");
    }
    return pipe;
}

RealtimeFaceBox boxOf(const cv::Rect& r, float score = 0.9f) {
    return RealtimeFaceBox{static_cast<float>(r.x), static_cast<float>(r.y),
                           static_cast<float>(r.width), static_cast<float>(r.height), score};
}

// 读取最新发布渲染帧（seq 从 0 起每次都能取到最新 copy）。
bool latestRender(Engine& engine, cv::Mat& out) {
    uint64_t seq = 0;
    return engine.getRenderFrame(out, seq);
}

// 测试子类：覆写发布前钩子（Engine::onBeforeRenderPublish，protected virtual），把引擎线程
// **确定性钉在**「快照拷贝后、发布前」窗口，供测试在该窗口内撤销身份。
// 第四轮审查 M3：以 protected virtual 空钩子取代此前公有的 setRenderPublishBarrier——后者注入
// 的 std::function 成员由引擎线程无锁读、setter 写，任何运行期调用即数据竞争 UB，且是公有误用面。
// armed 仅在受测帧置位，此前帧走「等价空钩子」直接返回，不影响正常渲染。
// 编译期开关：该钩子与 ~Engine() 的 virtual 仅在定义 RK_ENABLE_TEST_SEAMS 时存在（本文件编译进
// core_gtest_tests，该 target 由 CMakeLists.txt 定义此宏；生产构建不定义 → Engine 无 vptr）。
class BarrierEngine : public Engine {
public:
    std::mutex mu;
    std::condition_variable cv;
    bool armed = false;    // 仅在受测帧启用
    bool entered = false;  // 已进入发布前窗口并阻塞（测试据此判定已钉住）
    bool release = false;  // 测试线程置 true 放行

    // 第五轮审查 P1：第二个屏障，钉在**发射临界区内**（onBeforePublishLocked，此时已持 identityEmitMu_）。
    // 与上面的发布前屏障相互独立：用于验证「撤销无法插在最终校验与事件发射之间」。
    bool emitArmed = false;
    bool emitEntered = false;
    bool emitRelease = false;

protected:
    void onBeforeRenderPublish() override {
        std::unique_lock<std::mutex> lock(mu);
        if (!armed) return;  // 未启用：等价空钩子
        entered = true;
        cv.notify_all();
        // 有界等待：测试线程未及时放行（如前置断言失败提前退出）时超时返回，绝不永久钉死引擎线程。
        cv.wait_for(lock, kInterleaveWait, [&] { return release; });
    }

    void onBeforePublishLocked() override {
        std::unique_lock<std::mutex> lock(mu);
        if (!emitArmed) return;  // 未启用：等价空钩子
        emitEntered = true;
        cv.notify_all();
        cv.wait_for(lock, kInterleaveWait, [&] { return emitRelease; });
    }
};

}  // namespace

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

// ── 任务12 实时路径回归测试 ──

// 单脸局部框不产生第二身份：同一张脸的大框 + 被其包含的局部小框，经统一后处理
// （IoU NMS + 包含框抑制）只保留一个检测框，下游 TrackCoordinator 因此不会建成
// 两条轨迹/两个身份。同时输入帧不被修改。
TEST(EngineTest, RealtimeSingleFacePartialBoxYieldsOneIdentity) {
    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat before = frame.clone();
    const std::vector<RealtimeFaceBox> boxes = {
        boxOf(cv::Rect(100, 100, 200, 200), 0.90f),  // 整脸
        boxOf(cv::Rect(140, 140, 80, 80), 0.85f),    // 局部/嵌套（被包含且分数更低）
    };
    const auto faces = Engine::postProcessRealtimeBoxes(frame, boxes);
    ASSERT_EQ(faces.size(), 1u);
    EXPECT_EQ(faces[0].bbox, cv::Rect(100, 100, 200, 200));
    // 纯后处理不得修改输入帧
    const cv::Mat diff = before != frame;
    EXPECT_EQ(cv::countNonZero(diff.reshape(1)), 0);
}

// 输入帧不被修改：实时路径开启翻转 + 检测框绘制时，输入帧像素必须保持原样
// （翻转输出新 Mat；绘制前 clone 发布帧）。
TEST(EngineTest, RealtimeProcessingDoesNotModifyInputFrame) {
    cv::Mat input(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(7, 7, 7));
    cv::Mat before = input.clone();

    for (bool flip : {false, true}) {
        Engine engine;
        engine.setFlip(flip, false);
        auto det = std::make_shared<ScriptedDetector>();
        det->boxes = {boxOf(cv::Rect(100, 100, 200, 200))};
        engine.setRealtimePipeline(makeRtPipeline(det));
        for (int i = 0; i < 3; ++i) {
            EXPECT_TRUE(engine.processFrameOnce(input));
        }
    }
    const cv::Mat diff = before != input;
    EXPECT_EQ(cv::countNonZero(diff.reshape(1)), 0);
}

// 四边缘人物翻转固定：变换只在推理前应用一次（检测运行在变换后图像上），检测框即
// 显示坐标系。以白色标记模拟「人物」置于四角，开启水平翻转后，其跟踪框应等于按
// 任务1 mapRect 语义映射后的位置（左↔右镜像、上下不变）；若检测先于变换或框被
// 二次翻转，四角位置会漂移/不镜像，本用例即失败。
TEST(EngineTest, RealtimeFlipKeepsEdgeBoxesInDisplayCoordinates) {
    const int W = rk_core::config::FRAME_WIDTH;
    const int H = rk_core::config::FRAME_HEIGHT;
    const cv::Rect markers[4] = {
        cv::Rect(0, 0, 20, 20),          // 左上
        cv::Rect(W - 20, 0, 20, 20),     // 右上
        cv::Rect(0, H - 20, 20, 20),     // 左下
        cv::Rect(W - 20, H - 20, 20, 20) // 右下
    };

    FrameTransformConfig cfg;  // 与 Engine::preprocessFrame 的水平翻转配置一致
    cfg.target_mirrored_x = true;

    for (const cv::Rect& marker : markers) {
        cv::Mat input(H, W, CV_8UC3, cv::Scalar(0, 0, 0));
        input(marker).setTo(cv::Scalar(255, 255, 255));

        Engine engine;
        engine.setFlip(true, false);  // 水平翻转
        engine.setRealtimePipeline(makeRtPipeline(std::make_shared<EdgeMarkerDetector>()));
        for (int i = 0; i < 3; ++i) {  // stableFrames=3 达成确认
            engine.processFrameOnce(input);
        }

        const auto tracks = engine.getTrackViews();
        ASSERT_EQ(tracks.size(), 1u) << "marker=" << marker.x << "," << marker.y;
        const cv::Rect expected = FrameTransform::mapRect(marker, cfg, W, H);
        EXPECT_EQ(tracks[0].bbox, expected) << "marker=" << marker.x << "," << marker.y;
    }
}

// 实际空检测后旧框按新规则消失：已确认轨迹在真实空检测下，缺失 1 次仍在，
// 连续缺失 2 次删除（任务5 规则；空检测 detectionRan 必须为 true）。
TEST(EngineTest, RealtimeEmptyDetectionRemovesTrackPerRule) {
    Engine engine;
    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    engine.setRealtimePipeline(makeRtPipeline(det));

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(0, 0, 0));
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);
    ASSERT_EQ(engine.getTrackViews().size(), 1u);

    det->boxes.clear();  // 实际空检测（detect 成功但零框）
    engine.processFrameOnce(frame);
    EXPECT_EQ(engine.getTrackViews().size(), 1u);  // 缺失 1 次：已确认轨迹保留
    engine.processFrameOnce(frame);
    EXPECT_EQ(engine.getTrackViews().size(), 0u);  // 缺失 2 次：删除
}

// 模型不可用：继续预览（发布帧）但显式 missing_model，且不产生任何轨迹，
// 绝不静默回退 LBPH（BioAuth 未初始化也不会被调用）。
TEST(EngineTest, RealtimeMissingModelPreviewsWithExplicitReason) {
    Engine engine;
    auto det = std::make_shared<ScriptedDetector>();
    engine.setRealtimePipeline(makeRtPipeline(det, nullptr, /*reload=*/false));

    cv::Mat input(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(5, 5, 5));
    cv::Mat before = input.clone();
    EXPECT_TRUE(engine.processFrameOnce(input));
    EXPECT_EQ(engine.getRealtimeReason(), "missing_model");
    EXPECT_TRUE(engine.getTrackViews().empty());
    EXPECT_EQ(engine.getRealtimeGeneration(), 0u);

    cv::Mat out;
    EXPECT_TRUE(latestRender(engine, out));  // 仍发布预览帧
    const cv::Mat diff = before != input;
    EXPECT_EQ(cv::countNonZero(diff.reshape(1)), 0);
}

// 图库命中：快照含图库 + 嵌入时，实时路径回填身份（稳定 ID 非 Unknown），
// 证明「身份」确实由 RealtimeFacePipeline 产出的识别链路给出（而非 LBPH）。
TEST(EngineTest, RealtimeGalleryMatchAssignsIdentity) {
    auto index = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    std::vector<FaceSearchEntry> entries = {{"alice", std::vector<float>(512, 1.0f)}};
    ASSERT_TRUE(index->reset(std::move(entries), 512, err)) << err;

    Engine engine;
    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    engine.setRealtimePipeline(makeRtPipeline(det, index));

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);

    const auto tracks = engine.getTrackViews();
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks[0].stableId, "alice");
}

// 第四轮审查 M1：setRealtimePipeline 幂等。同对象重复注入（宿主配置刷新/重复激活同一管线）
// 不得清空在途稳定身份、不得递增代次——修复前无条件 revokeTrackIdentitiesLocked({}) 会把已确认的
// alice 清为 Unknown，须重新累计 stableFrames 才恢复，并使后续 injectGalleryToEngine(keepIds) 无物可保。
// 换到**不同**管线对象（= 换模型）时仍须完整撤销（保留轨迹与 bbox，不丢框），语义不变。
TEST(EngineTest, SetRealtimePipelineSameObjectIsIdempotent) {
    auto index = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    ASSERT_TRUE(index->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, err)) << err;

    Engine engine;
    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    auto pipe = makeRtPipeline(det, index);
    engine.setRealtimePipeline(pipe);

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);
    ASSERT_EQ(engine.getTrackViews().size(), 1u);
    ASSERT_EQ(engine.getTrackViews()[0].stableId, "alice");

    // 同对象重复注入：身份必须原样保留（幂等），不得清为 Unknown。
    engine.setRealtimePipeline(pipe);
    const auto kept = engine.getTrackViews();
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0].stableId, "alice") << "同对象重复注入不应抹掉在途稳定身份";

    // 换到另一条管线（不同对象 = 换模型）：完整撤销身份（语义不变），轨迹与 bbox 保留。
    auto pipe2 = makeRtPipeline(det, index);
    engine.setRealtimePipeline(pipe2);
    const auto after = engine.getTrackViews();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].stableId, "Unknown") << "换模型必须撤销旧模型下的稳定身份";
    EXPECT_GT(after[0].bbox.area(), 0);
}

// ── 第三波审查 #4 残留竞态：合并 API applyGalleryUpdate ──

// 合并 API 语义：applyGalleryUpdate 必须在**单次调用**内同时完成「换图库覆盖 + 精准撤销身份」，
// 两项效果均来自这一次调用，不存在「图库已换但身份未撤」/「身份已撤但图库未换」的可观测中间态。
TEST(EngineTest, ApplyGalleryUpdateSwapsGalleryAndRevokesAtomically) {
    auto index = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    std::vector<FaceSearchEntry> entries = {{"alice", std::vector<float>(512, 1.0f)}};
    ASSERT_TRUE(index->reset(std::move(entries), 512, err)) << err;

    Engine engine;
    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    engine.setRealtimePipeline(makeRtPipeline(det, index));

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);
    ASSERT_EQ(engine.getTrackViews().size(), 1u);
    ASSERT_EQ(engine.getTrackViews()[0].stableId, "alice");

    // 单次调用：换库（空库，已无 alice）+ 撤销（keepIds 空，等价删除末位人员）。
    auto rebuilt = std::make_shared<FaceSearchLinearIndex>();
    std::string err2;
    ASSERT_TRUE(rebuilt->reset({}, 512, err2)) << err2;
    engine.applyGalleryUpdate(rebuilt, {});

    // 效果① 身份撤销即时生效（保留轨迹与 bbox，不丢框）。
    const auto after = engine.getTrackViews();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].stableId, "Unknown");
    EXPECT_GT(after[0].bbox.area(), 0);

    // 效果② 新图库已生效——后续帧无法再匹配到已删除的 alice（认证路径读的是新覆盖库）。
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);
    const auto post = engine.getTrackViews();
    ASSERT_EQ(post.size(), 1u);
    EXPECT_EQ(post[0].stableId, "Unknown");
}

// 合并 API 的 keepIds 语义：命中者身份原样保留（多人场景删除/注册一人不抹掉他人），
// 未命中者清为 "Unknown"（保留框、不需重新累计 stableFrames）。
TEST(EngineTest, ApplyGalleryUpdateKeepsIdentitiesInKeepIds) {
    auto index = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    std::vector<FaceSearchEntry> entries = {{"alice", std::vector<float>(512, 1.0f)}};
    ASSERT_TRUE(index->reset(std::move(entries), 512, err)) << err;

    Engine engine;
    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(100, 100, 120, 160))};
    engine.setRealtimePipeline(makeRtPipeline(det, index));

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);
    ASSERT_EQ(engine.getTrackViews().size(), 1u);
    ASSERT_EQ(engine.getTrackViews()[0].stableId, "alice");

    // keepIds 命中 alice：换库 + 保留身份。
    auto keepIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string e2;
    ASSERT_TRUE(keepIdx->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, e2)) << e2;
    engine.applyGalleryUpdate(keepIdx, {"alice"});
    const auto kept = engine.getTrackViews();
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0].stableId, "alice");
    EXPECT_GT(kept[0].bbox.area(), 0);

    // keepIds 空：换空库 + 撤销（轨迹与 bbox 保留，仅身份清为 Unknown）。
    auto emptyIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string e3;
    ASSERT_TRUE(emptyIdx->reset({}, 512, e3)) << e3;
    engine.applyGalleryUpdate(emptyIdx, {});
    const auto cleared = engine.getTrackViews();
    ASSERT_EQ(cleared.size(), 1u);
    EXPECT_EQ(cleared[0].stableId, "Unknown");
    EXPECT_GT(cleared[0].bbox.area(), 0);
}

// 第三波审查 #4 残留竞态：applyGalleryUpdate（单临界区换库+撤销）与引擎帧处理并发。
// 反 flaky 与既有并发用例同策略（原子起跑栅栏 + 有限轮次 + join + 原子计数）：到达此处即证
// 全程无崩溃、无对 coordinator 内部状态的数据竞争；结束后再做确定性断言——换到空库并驱动帧后
// 不得出现「已删除者仍被认证」（alice 不应复现），即闭合「删除后仍认证」的残留窗口。
TEST(EngineTest, ConcurrentApplyGalleryUpdateAndFrameProcessingIsRaceFree) {
    auto aliceIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    std::vector<FaceSearchEntry> entries = {{"alice", std::vector<float>(512, 1.0f)}};
    ASSERT_TRUE(aliceIdx->reset(std::move(entries), 512, err)) << err;
    auto emptyIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string err2;
    ASSERT_TRUE(emptyIdx->reset({}, 512, err2)) << err2;

    Engine engine;
    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    engine.setRealtimePipeline(makeRtPipeline(det, aliceIdx));
    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));

    constexpr int kFrameRounds = 1000;
    constexpr int kUpdateRounds = 500;
    std::atomic<int> framesDone{0};
    std::atomic<int> updatesDone{0};
    std::atomic<bool> sawUnexpected{false};  // 仅允许 Unknown / alice（图库中唯一身份）
    std::atomic<bool> go{false};

    std::thread framer([&] {
        while (!go.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kFrameRounds; ++i) {
            engine.processFrameOnce(frame);  // 持 tracksMu_ 执行 update()（与撤销交错）
            for (const auto& v : engine.getTrackViews()) {
                if (v.stableId != "Unknown" && v.stableId != "alice") {
                    sawUnexpected.store(true, std::memory_order_relaxed);
                }
            }
            framesDone.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread updater([&] {
        while (!go.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kUpdateRounds; ++i) {
            if (i % 2 == 0) {
                engine.applyGalleryUpdate(emptyIdx, {});          // 换空库 + 撤销
            } else {
                engine.applyGalleryUpdate(aliceIdx, {"alice"});   // 换含 alice 库 + 保留
            }
            updatesDone.fetch_add(1, std::memory_order_relaxed);
        }
    });

    go.store(true, std::memory_order_release);
    framer.join();
    updater.join();

    EXPECT_EQ(framesDone.load(), kFrameRounds);
    EXPECT_EQ(updatesDone.load(), kUpdateRounds);
    EXPECT_FALSE(sawUnexpected.load());

    // 结束后确定状态：换到空库再驱动帧，已删除者 alice 不得复现（认证结论据新库得出）。
    engine.applyGalleryUpdate(emptyIdx, {});
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);
    const auto fin = engine.getTrackViews();
    ASSERT_EQ(fin.size(), 1u);
    EXPECT_EQ(fin[0].stableId, "Unknown");
}

// ── #1 注册采集接缝回归 ──

// 采集模式关闭时零回调；开启后对每个检测脸回调一次，且 embedding/bbox/关键点/质量/faceCount
// 字段正确透传（这是 EnrollmentSession 唯一的逐帧特征源）。
TEST(EngineTest, EnrollmentCaptureCallbacksOnlyWhenEnabled) {
    Engine engine;
    auto det = std::make_shared<ScriptedDetector>();
    RealtimeFaceBox b = boxOf(cv::Rect(200, 150, 160, 200), 0.9f);
    b.hasKeypoints = true;
    b.keypoints = {120.f, 140.f, 240.f, 140.f, 180.f, 180.f, 150.f, 220.f, 210.f, 220.f};
    det->boxes = {b};
    engine.setRealtimePipeline(makeRtPipeline(det));

    std::atomic<int> count{0};
    EnrollmentCaptureSample got;
    std::mutex mu;
    engine.setEnrollmentCaptureCallback([&](const EnrollmentCaptureSample& s) {
        std::lock_guard<std::mutex> lock(mu);
        got = s;
        count.fetch_add(1);
    });

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(90, 90, 90));

    // 关闭：无回调
    EXPECT_FALSE(engine.isEnrollmentCapture());
    engine.setEnrollmentCapture(false);
    engine.processFrameOnce(frame);
    EXPECT_EQ(count.load(), 0);

    // 开启：逐脸回调一次，字段正确
    engine.setEnrollmentCapture(true);
    EXPECT_TRUE(engine.isEnrollmentCapture());
    engine.processFrameOnce(frame);
    EXPECT_EQ(count.load(), 1);
    {
        std::lock_guard<std::mutex> lock(mu);
        EXPECT_EQ(got.embedding.size(), static_cast<std::size_t>(512));
        EXPECT_FLOAT_EQ(got.embedding[0], 1.0f);  // ConstantEmbedder 全 1
        EXPECT_EQ(got.bbox, cv::Rect(200, 150, 160, 200));
        EXPECT_EQ(got.faceCount, 1);
        EXPECT_FLOAT_EQ(got.detScore, 0.9f);
        EXPECT_EQ(got.frameWidth, rk_core::config::FRAME_WIDTH);
        EXPECT_EQ(got.frameHeight, rk_core::config::FRAME_HEIGHT);
        EXPECT_TRUE(got.hasLandmarks);
        EXPECT_FLOAT_EQ(got.landmarks[0], 120.f);
        EXPECT_FLOAT_EQ(got.landmarks[9], 220.f);
        // 常量灰度图：均值=90，Laplacian 方差=0
        EXPECT_FLOAT_EQ(got.grayMean, 90.0f);
        EXPECT_FLOAT_EQ(got.laplacianVariance, 0.0f);
        // 模板红线：调用方持有的副本可显式擦除
        std::fill(got.embedding.begin(), got.embedding.end(), 0.0f);
        EXPECT_FLOAT_EQ(got.embedding[0], 0.0f);
    }

    // 再次关闭后不再回调
    engine.setEnrollmentCapture(false);
    engine.processFrameOnce(frame);
    EXPECT_EQ(count.load(), 1);
}

// 模型未生效（无快照）时实时路径根本不检测：采集不得产出任何样本。
TEST(EngineTest, EnrollmentCaptureNotTriggeredWhenModelMissing) {
    Engine engine;
    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    engine.setRealtimePipeline(makeRtPipeline(det, nullptr, /*reload=*/false));

    std::atomic<int> count{0};
    engine.setEnrollmentCaptureCallback(
        [&](const EnrollmentCaptureSample&) { count.fetch_add(1); });
    engine.setEnrollmentCapture(true);

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(10, 10, 10));
    engine.processFrameOnce(frame);
    EXPECT_EQ(count.load(), 0);
    EXPECT_EQ(engine.getRealtimeReason(), "missing_model");
}

// 质量指标纯函数：常量图 Laplacian 方差为 0；中心亮点响应方差 > 0。
TEST(EngineTest, ComputeEnrollmentQualityMatchesJavaSemantics) {
    cv::Mat flat(4, 4, CV_8UC1, cv::Scalar(100));
    float m = 0.f;
    float s = 0.f;
    float v = 0.f;
    ASSERT_TRUE(Engine::computeEnrollmentQuality(flat, m, s, v));
    EXPECT_FLOAT_EQ(m, 100.0f);
    EXPECT_FLOAT_EQ(s, 0.0f);
    EXPECT_FLOAT_EQ(v, 0.0f);

    // 5x5：内部像素 3x3=9 个，中心亮点在 4 邻域上产生 1 个 400 与 4 个 -100 的响应，
    // 总体方差 = (400^2 + 4*100^2)/9 > 0。注意 3x3 图内部仅 1 个像素，单值总体方差
    // 恒等于 0（旧用例即因此误判为 0），故必须用能容纳多个内部像素的尺寸。
    cv::Mat edge(5, 5, CV_8UC1, cv::Scalar(0));
    edge.at<unsigned char>(2, 2) = 100;
    ASSERT_TRUE(Engine::computeEnrollmentQuality(edge, m, s, v));
    EXPECT_GT(v, 0.0f);

    cv::Mat empty;
    EXPECT_FALSE(Engine::computeEnrollmentQuality(empty, m, s, v));
}

// #1 对齐一致性回归：同一帧、同一检测（含 5 关键点）下，注册采集与身份匹配必须走同一
// 关键点仿射对齐。修复前身份路径强制 bbox fallback，注册模板与查库 probe 来自不同对齐，
// 同一人脸 cosine 可能跌破 identitySimilarity → 注册后识别不到（本用例会 RED）。
// 断言两层：① 身份路径产出的对齐输入与注册路径逐字节相同；② 用注册模板建库后能命中。
TEST(EngineTest, IdentityPathUsesKeypointAlignmentLikeEnrollment) {
    auto det = std::make_shared<ScriptedDetector>();
    RealtimeFaceBox b = boxOf(cv::Rect(200, 150, 160, 200), 0.9f);
    b.hasKeypoints = true;
    // 相对 bbox 倾斜的关键点：关键点仿射（相似变换）与 bbox 裁剪/resize 几何不同，
    // 使两种对齐在结构纹理上产生可区分的采样向量。
    b.keypoints = {140.f, 180.f, 250.f, 170.f, 200.f, 220.f, 160.f, 270.f, 230.f, 275.f};
    det->boxes = {b};

    auto emb = std::make_shared<ContentRecordingEmbedder>();

    // 棋盘纹理帧：任何对齐差异都会改变采样相位，避免常量图掩盖两种对齐的差异。
    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH, CV_8UC3);
    for (int y = 0; y < frame.rows; ++y) {
        cv::Vec3b* row = frame.ptr<cv::Vec3b>(y);
        for (int x = 0; x < frame.cols; ++x) {
            const unsigned char v =
                static_cast<unsigned char>(60 + (((x / 6) + (y / 6)) % 2) * 150);
            row[x] = cv::Vec3b(v, v, v);
        }
    }

    // ① 注册采集：关键点对齐产出模板 E。
    EnrollmentCaptureSample enrolled;
    std::mutex mu;
    std::atomic<int> count{0};
    {
        Engine e1;
        e1.setRealtimePipeline(makeRtPipeline(det, nullptr, /*reload=*/true, emb));
        e1.setEnrollmentCaptureCallback([&](const EnrollmentCaptureSample& s) {
            std::lock_guard<std::mutex> lock(mu);
            enrolled = s;
            count.fetch_add(1);
        });
        e1.setEnrollmentCapture(true);
        e1.processFrameOnce(frame);
    }
    ASSERT_EQ(count.load(), 1);
    ASSERT_EQ(enrolled.embedding.size(), 512u);

    // ② 识别：以注册模板建库，身份路径跑满 stableFrames=3 帧。
    auto index = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    std::vector<FaceSearchEntry> entries = {{"alice", enrolled.embedding}};
    ASSERT_TRUE(index->reset(std::move(entries), 512, err)) << err;

    {
        Engine e2;
        e2.setRealtimePipeline(makeRtPipeline(det, index, /*reload=*/true, emb));
        for (int i = 0; i < 3; ++i) e2.processFrameOnce(frame);
        const auto tracks = e2.getTrackViews();
        ASSERT_EQ(tracks.size(), 1u);
        EXPECT_EQ(tracks[0].stableId, "alice");
    }

    // ③ 核心断言：注册 1 次 + 身份 3 次对齐输入，必须全部逐字节相同（同一对齐）。
    std::lock_guard<std::mutex> lock(emb->mu);
    ASSERT_EQ(emb->alignedInputs.size(), 4u);
    for (std::size_t i = 1; i < emb->alignedInputs.size(); ++i) {
        ASSERT_EQ(emb->alignedInputs[i].size(), emb->alignedInputs[0].size());
        ASSERT_EQ(emb->alignedInputs[i].type(), emb->alignedInputs[0].type());
        EXPECT_DOUBLE_EQ(cv::norm(emb->alignedInputs[i], emb->alignedInputs[0], cv::NORM_INF), 0.0);
    }
}

// ── 确定性交错（上轮未覆盖项 C）：锁定具体交错点，而非「起跑栅栏 + 有限轮次」冒烟 ──

// 读路径确定性交错：图库更新发生在「assignRealtimeIdentities 读库之后、trackFaces 提交之前」。
// 用阻塞式 embedder 把引擎线程**确定性钉在**该窗口（embed 恰在记录 frameGalleryGeneration_ 之后、
// trackFaces 之前被调用），期间 applyGalleryUpdate（换空库 + 撤销身份），放行后 trackFaces 的
// **代次作废**必须丢弃本帧据旧库得出的认证结论 → 提交后该帧 stableId 为 "Unknown"。
// 若 trackFaces 缺失 frameGalleryGeneration_ 比对，旧库结论（alice）会把身份重新写回轨迹（RED）。
TEST(EngineTest, GalleryUpdateDuringFrameDropsStaleIdentityAtCommit) {
    auto aliceIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    ASSERT_TRUE(aliceIdx->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, err)) << err;
    auto emptyIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string e2;
    ASSERT_TRUE(emptyIdx->reset({}, 512, e2)) << e2;

    auto emb = std::make_shared<BlockingEmbedder>();
    Engine engine;
    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    engine.setRealtimePipeline(makeRtPipeline(det, aliceIdx, /*reload=*/true, emb));

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);  // 先确认 alice（stableFrames=3）
    ASSERT_EQ(engine.getTrackViews().size(), 1u);
    ASSERT_EQ(engine.getTrackViews()[0].stableId, "alice");

    // 只阻塞下一帧的 embed：引擎线程由此停在「读库后、提交前」。
    {
        std::lock_guard<std::mutex> lock(emb->mu);
        emb->blockNext = true;
    }
    std::thread framer([&] { engine.processFrameOnce(frame); });
    bool enteredOk = false;
    {
        std::unique_lock<std::mutex> lock(emb->mu);
        // 有界等待引擎进入 embed（钉住窗口）。超时即视作未建立窗口，后续以断言失败收尾。
        enteredOk = emb->cv.wait_for(lock, kInterleaveWait, [&] { return emb->entered; });
    }

    if (enteredOk) {
        // 窗口内：换空库 + 撤销身份（同时递增 galleryGeneration_ 与 identityVersion_）。
        engine.applyGalleryUpdate(emptyIdx, {});
    }

    // 统一放行：enteredOk=false 时也放行，保证 framer 必定退出（避免 join 永久阻塞/线程析构
    // terminate）。随后以断言给出干净诊断。
    {
        std::lock_guard<std::mutex> lock(emb->mu);
        emb->released = true;
    }
    emb->cv.notify_all();
    framer.join();
    ASSERT_TRUE(enteredOk) << "引擎线程未在超时内进入 embed（实时路径未调用嵌入，确定性窗口未建立）";

    // 提交点代次作废：本帧据旧库得出的 alice 结论被丢弃，轨迹身份为 Unknown（保留框与计数）。
    const auto tracks = engine.getTrackViews();
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks[0].stableId, "Unknown");
}

// 发布路径确定性交错：撤销发生在 renderResults「快照拷贝之后、发布之前」。
// renderResults 在 tracksMu_ 内拷贝 currentTracks_、解锁后绘制并发布；用 BarrierEngine 覆写
// onBeforeRenderPublish 把引擎线程钉在该窗口，期间 applyGalleryUpdate 撤销并递增 identityVersion_，
// 放行后发布前校验须清空本帧身份 → 发布帧以**红框**（"Unknown"）绘制而非绿框，且不发出旧 VERIFIED。
// 若缺失 identityVersion_ 校验，旧副本（alice）会以绿框绘制并作为 VERIFIED 发布（RED）。
TEST(EngineTest, RevocationBetweenSnapshotAndPublishDropsStaleVerified) {
    auto aliceIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    ASSERT_TRUE(aliceIdx->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, err)) << err;

    BarrierEngine engine;
    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    engine.setRealtimePipeline(makeRtPipeline(det, aliceIdx));

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);  // 确认 alice
    ASSERT_EQ(engine.getTrackViews().size(), 1u);
    ASSERT_EQ(engine.getTrackViews()[0].stableId, "alice");

    auto emptyIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string e2;
    ASSERT_TRUE(emptyIdx->reset({}, 512, e2)) << e2;

    // 启用发布屏障（仅对受测帧生效）：把引擎线程钉在「快照拷贝后、发布前」窗口。
    {
        std::lock_guard<std::mutex> lock(engine.mu);
        engine.armed = true;
    }

    std::thread framer([&] { engine.processFrameOnce(frame); });
    bool enteredOk = false;
    {
        std::unique_lock<std::mutex> lock(engine.mu);
        // 有界等待引擎到达发布前窗口。超时即视作未建立窗口，后续以断言失败收尾。
        enteredOk = engine.cv.wait_for(lock, kInterleaveWait, [&] { return engine.entered; });
    }

    if (enteredOk) {
        // 此刻引擎已拷贝 currentTracks_（含 alice）并停在发布前；在窗口内撤销身份。
        engine.applyGalleryUpdate(emptyIdx, {});
    }

    // 统一放行：enteredOk=false 时也放行，保证 framer 必定退出（避免 join 永久阻塞/线程析构 terminate）。
    {
        std::lock_guard<std::mutex> lock(engine.mu);
        engine.release = true;
    }
    engine.cv.notify_all();
    framer.join();
    ASSERT_TRUE(enteredOk) << "引擎线程未在超时内到达发布屏障（渲染路径未进入发布窗口）";

    // 发布前校验生效：本帧身份被清为 Unknown（对外视图一致），发布帧以红框绘制。
    const auto tracks = engine.getTrackViews();
    ASSERT_EQ(tracks.size(), 1u);
    ASSERT_EQ(tracks[0].stableId, "Unknown");

    cv::Mat published;
    ASSERT_TRUE(latestRender(engine, published));
    const int sx = tracks[0].bbox.x + tracks[0].bbox.width / 2;
    const int sy = tracks[0].bbox.y;  // 顶边：红框 R 通道 ≈255，绿框 G 通道 ≈255
    ASSERT_LT(sy, published.rows);
    ASSERT_LT(sx, published.cols);
    const cv::Vec3b border = published.at<cv::Vec3b>(sy, sx);
    EXPECT_GT(static_cast<int>(border[2]), 200) << "已撤销身份不应以认证绿框发布";
    EXPECT_LT(static_cast<int>(border[1]), 100);
}

// 第五轮审查 P1（发布路径**最终**窗口）：撤销不得插在「最终身份版本校验」与「事件发射」之间。
//
// 缺陷：修复前 renderResults 的最终校验与其后的 publisher_->publish() 之间仍有缝隙——JNI 线程可在
// 缝隙内完成 applyGalleryUpdate 并**返回**，此后本帧仍按旧副本发出 VERIFIED <已删除的人>。
// 「检后再发」不足以闭合，因为检查与发射不是原子的。
//
// 本用例用 onBeforePublishLocked 把引擎线程钉在**发射临界区内**（此时已持 identityEmitMu_），
// 另一线程随即调用 applyGalleryUpdate：修复后它必须等到临界区结束才能返回，故放行前 revokeDone
// 必为 false（**缺该门时 applyGalleryUpdate 立即返回 → RED**）。放行并 join 后，再逐条检查回调：
// 「撤销返回之后」发出的任何事件都不得含该身份。
TEST(EngineTest, RevocationCannotReturnBetweenFinalCheckAndEmit) {
    auto aliceIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    ASSERT_TRUE(aliceIdx->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, err)) << err;

    BarrierEngine engine;
    std::atomic<bool> revokeDone{false};
    std::mutex logMu;
    std::vector<std::pair<bool, std::string>> events;  // first = 「撤销返回之后」才发出
    engine.setOnResultCallback([&](const std::string& s) {
        std::lock_guard<std::mutex> lk(logMu);
        events.emplace_back(revokeDone.load(), s);
    });

    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    engine.setRealtimePipeline(makeRtPipeline(det, aliceIdx));

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH, CV_8UC3,
                  cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);  // 确认 alice
    ASSERT_EQ(engine.getTrackViews().size(), 1u);
    ASSERT_EQ(engine.getTrackViews()[0].stableId, "alice");

    auto emptyIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string e2;
    ASSERT_TRUE(emptyIdx->reset({}, 512, e2)) << e2;

    {
        std::lock_guard<std::mutex> lock(engine.mu);
        engine.emitArmed = true;  // 仅对受测帧启用「发射临界区」屏障
    }

    std::thread framer([&] { engine.processFrameOnce(frame); });
    bool enteredOk = false;
    {
        std::unique_lock<std::mutex> lock(engine.mu);
        enteredOk = engine.cv.wait_for(lock, kInterleaveWait, [&] { return engine.emitEntered; });
    }

    std::atomic<bool> revokeStarted{false};
    std::thread revoker([&] {
        revokeStarted.store(true);
        if (enteredOk) engine.applyGalleryUpdate(emptyIdx, {});
        revokeDone.store(true);
    });

    bool blockedWhilePinned = false;
    if (enteredOk) {
        // 先等 revoker **真正开始执行**（有界），否则慢机器上它可能尚未被调度、revokeDone 恒为 false
        // ——那样「未加门」也会通过（false pass，A2' Med / A3' δ3）。此后 200ms 足以让「未加门」的
        // 实现（立即返回）暴露；加了门则必然阻塞，等待只会令本断言更可靠。
        const auto deadline = std::chrono::steady_clock::now() + kInterleaveWait;
        while (!revokeStarted.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        blockedWhilePinned = !revokeDone.load();
    }

    // 统一放行（enteredOk=false 时也放行，保证 framer/revoker 必定退出，避免 join 阻塞）。
    {
        std::lock_guard<std::mutex> lock(engine.mu);
        engine.emitRelease = true;
    }
    engine.cv.notify_all();
    framer.join();
    revoker.join();

    ASSERT_TRUE(enteredOk) << "引擎线程未在超时内进入发射临界区";
    EXPECT_TRUE(blockedWhilePinned)
            << "撤销在发射临界区结束前即已返回：最终校验与事件发射未被同一门序列化";

    ASSERT_TRUE(revokeDone.load());
    // 撤销返回后**再驱动一帧**，并用**确定性**断言锁住「该身份不会回来」。此前这里是一条恒空转的
    // 「承重断言」（A2' Med / A3' δ2 实测 AFTER_REVOKE_CHECKED=0）——revoker 被门阻塞到 framer
    // 结束才置 revokeDone，events 中不存在 first==true 的元素，循环体零执行；既不判别也不可靠。
    engine.processFrameOnce(frame);
    const auto afterRevoke = engine.getTrackViews();
    ASSERT_EQ(afterRevoke.size(), 1u);
    EXPECT_EQ(afterRevoke[0].stableId, "Unknown") << "撤销返回后不得再恢复旧身份";

    // 事件侧补充检查：撤销返回后记录到的**任何**事件都不得含该身份。**若**为空说明节流期内未产出
    // 文本（正常），故此处刻意不要求「至少一条」——强求会造成抖动，见上。
    {
        std::lock_guard<std::mutex> lk(logMu);
        for (const auto& ev : events) {
            if (ev.first) {
                EXPECT_EQ(ev.second.find("VERIFIED alice"), std::string::npos)
                        << "撤销返回后仍发出旧身份事件: " << ev.second;
            }
        }
    }
}

// FACE-PIPE-015：把发射门「免死锁契约」从注释升级为可执行约束（重入检测 + 拒绝）。
//
// 契约：renderResults 持 identityEmitMu_ 期间经 publisher_->publish 调用结果回调；回调**不得同步
// 回入**任何取 tracksMu_ 的公开 API——穷尽集为 5 个：run / processFrameOnce / setRealtimePipeline /
// applyGalleryUpdate / getTrackViews（见 Engine.h 的同名条目）——否则
// 与撤销路径（revokeTrackIdentitiesLocked：tracksMu_ → identityEmitMu_）构成 ABBA 死锁。修复前该约定
// 仅存在于注释，无任何编译期/运行期护栏；现在这些入口在入口处检测「本线程正处发射门内」并拒绝执行。
//
// 确定性回归：覆写 onBeforePublishLocked（测试接缝，此时引擎线程**必已持 identityEmitMu_**），在**同一
// 线程**内同步调用 applyGalleryUpdate，断言其**返回失败（false）**而非挂死。
//
// ⚠️ **已知未覆盖（子代理3 用探针实测确认）**：`run()` 的重入守卫**没有判别用例**——注释掉 `Engine::run()`
// 顶部那行守卫，本套件仍 88 通过 / 0 失败。
//
// 这是**成本收益取舍，不是「做不到」**。子代理4 给出了两条**安全且确定**的测法（其仅为构造性论证，未实做）：
//   · 设计 A（需一个极小的测试接缝）：在 `run()` 的守卫之后、`isRunning_ = true` **之前**加一个
//     `RK_ENABLE_TEST_SEAMS` 下的虚接缝（与既有 onBeforeRenderPublish / onBeforePublishLocked 同款），
//     测试子类覆写它并抛哨兵异常。无守卫时该异常在**循环外的无 try 区**抛出 → 穿透到 worker 的
//     `catch(...)` → `run()` **根本不进循环** ⇒ 无自旋、无 detach、无泄漏，断言「接缝未被触发」即可判别。
//     代价 = 1 个虚函数 + 1 行调用（生产不定义宏 ⇒ 零成本）。
//   · 设计 B（零生产改动）：让 `run()` 无帧可消费（不接相机）⇒ 恒走 `processed=false → sleep → continue`，
//     永不触碰 identityEmitMu_；另起守护线程限时调 `stop()`（只置原子、不取两把锁）以有界终止，再用
//     gtest `CaptureStdout()` 判「守卫之后那行 `Engine started.` 是否输出」——**文本断言，非计时断言**。
//   ⇒ 之所以仍**不做**：该守卫**生产不可达**（结果回调不可能同步调用阻塞主循环），且 5 个入口共用同一行
//   守卫形态、已由本用例的 `applyGalleryUpdate` 这一代表覆盖；为一个不可达路径引入新生产接缝（设计 A 要
//   改头文件）或做 stdout 捕获（设计 B 较繁琐），收益不抵成本。**若该路径将来变得可达，按设计 A 补测。**
//
// ℹ️ RED 的**平台差异**（别照字面理解本用例的"死锁"描述）：MSVC 的 `std::mutex` 对**同线程重入**会**抛**
// `system_error`（"resource deadlock would occur"）而非挂死 ⇒ 被 worker 的 `catch(...)` 接住、照常置
// workerDone，RED 收敛为**干净的断言失败**、**不触发 detach**；只有 libstdc++（Linux，非本项目门禁）才是
// 真挂死并走 detach 分支。

// 有界性（关键：保证回归时为「用例失败」而非「套件挂死」）：重入检测基于 thread_local，重入必须发生在
// **同一线程**（钩子内部），故**不能**把重入调用挪到别的线程再等超时（那样标记不生效、测不到）。做法是
// 把**驱动一帧**整体放在独立 worker 线程；主线程对 worker 完成做**有界等待**——超时即 ADD_FAILURE 并
// **detach 卡住的 worker、不再析构 Engine（泄漏之）**：卡住的线程仍持有泄漏 Engine 内部互斥量，但不触碰
// 已释放内存，套件得以前进。故 RED 表现为「本用例失败、套件继续」，而非 CI job 超时。
class ReentryProbeEngine : public Engine {
public:
    std::atomic<bool> reentryArmed{false};   // 仅对受测帧启用重入
    std::atomic<bool> reentryDone{false};    // 钩子内 applyGalleryUpdate 已返回
    std::atomic<bool> reentryResult{true};   // 其返回值（预期 false）
    std::shared_ptr<const FaceSearchLinearIndex> gallery;  // 钩子内换入的库（内容与本用例无关）

protected:
    void onBeforePublishLocked() override {
        if (!reentryArmed.load()) return;  // 其余帧走等价空钩子
        // 同一线程（引擎线程）、持 identityEmitMu_：同步回入取 tracksMu_ 的公开 API。
        const bool ok = applyGalleryUpdate(gallery, {});
        reentryResult.store(ok);
        reentryDone.store(true);
    }
};

TEST(EngineTest, ResultCallbackReentryFromEmitGateIsRejectedNotDeadlocked) {
    auto aliceIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    ASSERT_TRUE(aliceIdx->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, err)) << err;

    auto emptyIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string e2;
    ASSERT_TRUE(emptyIdx->reset({}, 512, e2)) << e2;

    // 堆分配：RED（未拒绝）时 worker 会卡死，需 detach 并**泄漏**本对象以保证套件存活。
    ReentryProbeEngine* engine = new ReentryProbeEngine();

    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    ASSERT_TRUE(engine->setRealtimePipeline(makeRtPipeline(det, aliceIdx)));

    const cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH, CV_8UC3,
                        cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) engine->processFrameOnce(frame);  // 先确认 alice
    ASSERT_EQ(engine->getTrackViews().size(), 1u);
    ASSERT_EQ(engine->getTrackViews()[0].stableId, "alice");

    engine->gallery = emptyIdx;
    engine->reentryArmed.store(true);

    // 有界驱动：帧处理（含钩子内重入）在独立 worker；主线程限时等待其完成。
    // ⚠️ **超时分支会 detach，detach 后 worker 仍可能恢复运行**，故它触及的一切都不得是本栈局部量：
    //   · workerDone 因此**堆分配 + shared_ptr 按值捕获**（worker 持一份引用计数 ⇒ 本函数返回后对象仍存活）；
    //   · frame 按值捕获（cv::Mat 拷贝持有数据引用，数据生命周期由引用计数保证）；
    //   · engine 是堆指针，且超时分支**刻意泄漏**（其内存仍被 detach 线程引用，不得析构）。
    //（A2 审计 Med-1：先前把 workerDone 写成栈上变量并按引用捕获，detach 后会写已销毁的栈帧，
    //  可随机打坏**其它**用例；且该风险不限于 RED——GREEN 下 worker 超过 kInterleaveWait 同样会 detach。）
    auto workerDone = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([engine, frame, workerDone] {
        // 吞掉 worker 内逃逸的异常：RED（未拒绝重入）时同一线程二次取 identityEmitMu_，MSVC 的
        // std::mutex 会**抛** system_error（"resource deadlock would occur"，非挂死；libstdc++ 上则是
        // 真挂死）。若任其逃出 worker 线程 → std::terminate() 中止整个进程（非「用例失败」）。此处
        // 捕获后照常置 workerDone，使 RED 收敛为**确定性断言失败**而非进程中止；真挂死的平台则由
        // 下方有界等待兜底。
        try {
            engine->processFrameOnce(frame);
        } catch (...) {
        }
        workerDone->store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + kInterleaveWait;
    while (!workerDone->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!workerDone->load()) {
        // RED（无重入检测）：worker 在 ABBA 死锁上永久阻塞。有界放弃以保套件存活：detach 而非 join
        //（join 会永久阻塞），并**泄漏** engine（其内存仍被 detach 线程引用，不得析构）。
        ADD_FAILURE() << "重入未被拒绝：worker 在超时内未返回（疑似发射门 ABBA 死锁）";
        worker.detach();
        return;
    }
    worker.join();

    EXPECT_TRUE(engine->reentryDone.load());
    EXPECT_FALSE(engine->reentryResult.load())
            << "从结果回调同步重入 applyGalleryUpdate 应被拒绝（返回 false）而非死锁";

    // 被拒绝的调用不得产生副作用：身份仍在（未换库、未撤销）。
    const auto post = engine->getTrackViews();
    ASSERT_EQ(post.size(), 1u);
    EXPECT_EQ(post[0].stableId, "alice");

    delete engine;
}

