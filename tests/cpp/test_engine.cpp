#include <gtest/gtest.h>

// flip 应用与检测框绘制提取为 Engine 公共静态纯函数（applyFlip/drawTracks），
// 本测试直接验证其不变量，无需 private 访问。
#include "Engine.h"
#include "Config.h"
#include "FaceSearch.h"

#include <atomic>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rk_core;

namespace {

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
        EXPECT_GT(got.areaRatio, 0.0f);
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
