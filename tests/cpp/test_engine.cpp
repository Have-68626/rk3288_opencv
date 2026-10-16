#include <gtest/gtest.h>

// flip 应用与检测框绘制提取为 Engine 公共静态纯函数（applyFlip/drawTracks），
// 本测试直接验证其不变量，无需 private 访问。
#include "Engine.h"
#include "Config.h"
#include "FaceSearch.h"
// Engine::noteEmbedError 的限频行为用例需要捕获落盘日志：NativeLog 已由本 target
// （core_gtest_tests，见 CMakeLists.txt 的 RK_CORE_LITE_SOURCES）编译链接。
#include "NativeLog.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
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
std::string rtManifest(const std::string& space = "") {
    std::string manifest =
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
    if (!space.empty()) {
        manifest.pop_back();
        manifest += ",\"templateSpaceId\":\"" + space + "\"}";
    }
    return manifest;
}

// 可脚本化检测器：boxes 即本次返回的检测框（空 = 实际空检测）。
class ScriptedDetector : public IRealtimeDetector {
public:
    std::vector<RealtimeFaceBox> boxes;
    bool fail = false;
    int delayMs = 0;
    bool detect(const cv::Mat&, std::vector<RealtimeFaceBox>& out, std::string& err) override {
        if (fail) {
            err = "runtime_init_failed";
            return false;
        }
        if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        out = boxes;
        return true;
    }
    const char* name() const override { return "scripted"; }
};

class BlockingDetector : public ScriptedDetector {
public:
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    bool detect(const cv::Mat& frame, std::vector<RealtimeFaceBox>& out, std::string& err) override {
        {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [this] { return released; });
        }
        return ScriptedDetector::detect(frame, out, err);
    }
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

// 双色调嵌入：按对齐脸灰度均值二值化为两种可区分的**单位向量**，用于构造「两张不同身份
// 的人脸」场景（ConstantEmbedder 对两张脸给同一向量，无法区分 alice/bob）。
// 亮区 → a 方向（512 维各 1/√512）；暗区 → b 方向（前 256 维各 1/16）。
// ⚠️ 必须输出**单位向量**：FaceSearchOptions::assumeL2Normalized 默认为 true，此时
//    FaceSearch.cpp 的 cosineSimilarity 直接返回**原始点积**（省去两次开方），隐含前提是
//    库条目与查询都已 L2 归一化。若这里输出非单位向量（例如全 1），两个模板与查询的点积
//    会与模长纠缠、排序不再是余弦序（本用例初版即因此让暗区脸误配到排前的 alice）。
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
            const float kBright = 0.044194174f;  // 1/√512；512·k² = 1
            for (float& v : out) v = kBright;    // 亮区 → alice 模板
        } else {
            const float kDark = 0.0625f;         // 1/16；256·k² = 1
            for (int i = 0; i < 256; ++i) out[static_cast<std::size_t>(i)] = kDark;  // 暗区 → bob 模板
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
    std::shared_ptr<IRealtimeEmbedder> embedder = nullptr,
    const std::string& space = "") {
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
        pipe->reload(rtManifest(space), "", "arm64-v8a", "");
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

// ── noteEmbedError 限频行为用例的替身与日志读取工具（范式抄自 test_native_log_redaction.cpp）──

// 判别性测试替身：在**同一次** captureEnrollmentFaces 调用（⇒ 同一条限频时间窗，窗口 5s）
// 内制造两条**文本不同**的 err ——
//   · 第 1 次 embed 返回 false 且给出非空 err（非空才会被 noteEmbedError 记录）⇒ .../embed 站点；
//   · 第 2 次 embed 返回 true 但产出**维度不符**的向量（256 ≠ 512）⇒ .../dim 站点。
// 这正是闸门修复所针对的形态：逐帧路径上多张脸给出**不同** err 文本。纯时间窗闸门 ⇒ 窗内只放行
// 一条；回退成「文本相同才抑制」⇒ 两条都放行。故本替身让「文本不同」成为可判别的行为差异。
class TwoTextErrorEmbedder : public IRealtimeEmbedder {
public:
    std::atomic<int> calls{0};
    bool embed(const cv::Mat&, std::vector<float>& out, std::string& err) override {
        if (calls.fetch_add(1) == 0) {
            err = "vitest-embed-alpha";  // 与下方维度文本不同；不含敏感 KV，不会被日志脱敏改写
            return false;                // → captureEnrollmentFaces/embed 站点
        }
        out.assign(256, 0.5f);           // 维度不符（kEmbeddingDim == 512）
        return true;                     // → captureEnrollmentFaces/dim 站点
    }
    int embeddingDim() const override { return 512; }
    const char* name() const override { return "two_text_err"; }
};

// 跨进程唯一临时目录后缀（同 test_native_log_redaction.cpp 约定）
std::string uniqueSuffix() {
    return "_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

// 读取文件全部内容
std::string readFileAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 统计 needle 在 hay 中**非重叠**出现的次数（逐字节，用于数日志行）。
int countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
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

// FACE-TRANS-012：flipGeneration_ 只在**变换配置真的变化**时递增（setFlip / setInputMirroredX 均
// 先 exchange 取旧值，仅当新值 != 旧值才 fetch_add；该计数供状态区诊断展示）。
// 判别点：删掉 setFlip 里的 `if (oldX != flipX || oldY != flipY)`（改成无条件 fetch_add）
//        ⇒ 第二段「同值重复设置不得递增」的断言翻红。
// 注：本成员与 FrameTransformConfig::generation **无任何代码关联**——后者在生产侧恒为 0、
//     无人写读（见 FrameTransform.h 文件头的 ⚠️ 说明），故不可断言二者一致。
TEST(EngineTest, FlipGenerationIncrementsOnlyOnRealChange) {
    Engine engine;  // 只需默认构造：setFlip 只触碰原子成员
    EXPECT_EQ(engine.getFlipGeneration(), 0u);

    engine.setFlip(true, false);  // 首次真实变化
    EXPECT_EQ(engine.getFlipGeneration(), 1u);
    engine.setFlip(true, false);  // 同值重复设置：幂等，不增
    EXPECT_EQ(engine.getFlipGeneration(), 1u) << "同值重复设置不得递增";
    engine.setFlip(false, false);  // 再次真实变化
    EXPECT_EQ(engine.getFlipGeneration(), 2u);
}

// FACE-TRANS-012：输入镜像标志与 setFlip 共用同一代次计数器（二者都会改变实际水平变换：
// effective = input XOR target）。
// 判别点：删掉 setInputMirroredX 里的 `if (old != mirrored)`（改成无条件 fetch_add）
//        ⇒ 第二段「同值重复设置仍是 2」的断言翻红。
TEST(EngineTest, InputMirroredXSharesFlipGenerationCounter) {
    Engine engine;
    engine.setFlip(true, false);       // 1
    engine.setInputMirroredX(true);    // 2
    EXPECT_EQ(engine.getFlipGeneration(), 2u);
    engine.setInputMirroredX(true);    // 同值重复设置：不增
    EXPECT_EQ(engine.getFlipGeneration(), 2u) << "同值重复设置不得递增";
    engine.setInputMirroredX(false);   // 真实变化
    EXPECT_EQ(engine.getFlipGeneration(), 3u);
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

// ★ 2026-09-28 新增（外部审查 [P2]「异常检测框可触发整数转换或加法溢出」的**判别用例**）。
// 缺陷：修复前本函数只挡 NaN/Inf 就把 float **直接**转 int（C++ `[conv.fpint]`：截断值不可表示即
//   **UB**），而 SCRFD 的 distance/delta 回归分量**无上界** ⇒ 畸形 / 敌意模型可给出 ~1e9 量级的
//   **有限**值。修复为「先夹进帧边界、后转换」（空帧时夹进 ±INT_MAX/2，保证端点差也可表示）。
// 判别力（**已按对抗性核查更正**，初版注释不成立，勿按初版读）：初版写「修复前 `static_cast<int>(3e9f)`
//   得到 INT_MIN ⇒ bbox.x 明显不同 ⇒ 可判别」——**该推理错的不是事实而是「能否被观测到」**：
//   旧实现下这四个框经下游 `FacePostProcessor` 裁剪后宽度全为 0、被**全部丢弃** ⇒ `faces` 为空
//   ⇒ 下面那个 `for` 循环**一次都不执行** ⇒ 所有断言**恒真**、用例在旧实现上照样绿（判别力为零）。
//   ⇒ **真正的判别点是「有没有框活下来」**，故下面补 `ASSERT_GE(faces.size(), 1u)` 作前置锚。
//   （对抗性代理用独立探针实测：旧实现 survivors=0 / 新实现 survivors=2。）
TEST(EngineTest, RealtimeOutOfRangeBoxesAreClampedNotUb) {
    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(0, 0, 0));
    const int W = frame.cols;
    const int H = frame.rows;

    const auto mk = [](float x, float y, float w, float h, float s) {
        RealtimeFaceBox b;
        b.x = x;
        b.y = y;
        b.width = w;
        b.height = h;
        b.score = s;
        return b;
    };
    const std::vector<RealtimeFaceBox> boxes = {
        mk(3e9f, 10.0f, 100.0f, 100.0f, 0.9f),    // x 远超 int 域
        mk(-3e9f, 10.0f, 100.0f, 100.0f, 0.8f),   // x 远低于 int 域
        mk(-2e9f, 10.0f, 4e9f, 100.0f, 0.7f),     // 端点差 6e9（> INT_MAX）
        mk(10.0f, 10.0f, 3e9f, 3e9f, 0.6f),       // 宽高巨大
    };
    const auto faces = Engine::postProcessRealtimeBoxes(frame, boxes);

    // ★ 前置锚（本用例**唯一的判别点**，见方法上方说明）：修复后必须**有框活下来**。
    //   旧实现下这四个框会被下游裁剪成宽度 0 而**全部丢弃** ⇒ 本断言红；若删掉它，下面的循环
    //   在旧实现上根本不执行、整条用例退化为恒真（这正是本用例初版的缺陷）。
    ASSERT_GE(faces.size(), 1u)
        << "修复后极值框应被夹进帧内并存活；若为 0 条，说明它们仍被下游丢弃（= 未夹取）";

    // 不断言 NMS 之后剩几条（极值框之间的 IoU 关系不稳定、不是本用例的判别对象），
    // 只断言**每一条**都落在帧内 —— 这正是「先约束后转换」的语义。
    for (const auto& f : faces) {
        EXPECT_GE(f.bbox.x, 0);
        EXPECT_GE(f.bbox.y, 0);
        EXPECT_LE(f.bbox.x, W);
        EXPECT_LE(f.bbox.y, H);
        EXPECT_GE(f.bbox.width, 0);
        EXPECT_GE(f.bbox.height, 0);
        // 用 64 位算端点，避免**测试自身**在未修复的产物上溢出
        EXPECT_LE(static_cast<long long>(f.bbox.x) + f.bbox.width, W);
        EXPECT_LE(static_cast<long long>(f.bbox.y) + f.bbox.height, H);
    }
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

// P9/A1：生产预处理路径的镜像接线 = input XOR target。setInputMirroredX 表示**输入源已镜像**
//（相机元数据），setFlip 表示**目标镜像**（用户偏好）；二者 XOR 决定实际水平翻转。
// 既有 RealtimeFlipKeepsEdgeBoxesInDisplayCoordinates 只覆盖 (input=F,target=T) 一格；本用例补
// (input=T,target=F)→翻 与 (input=T,target=T)→不翻 两格，端到端走 preprocessFrame（生产接线）。
// 判别点：若 preprocessFrame 忽略 inputMirroredX_（把 input 恒置 false），则 (T,F) 格不再翻转，
//        marker 不镜像 ⇒ 与 mapRect 期望不符 ⇒ 本断言翻红。
TEST(EngineTest, InputMirroredXProductionWiringFollowsXor) {
    const int W = rk_core::config::FRAME_WIDTH;
    const int H = rk_core::config::FRAME_HEIGHT;
    const cv::Rect marker(0, 0, 20, 20);  // 左上角（靠左、非对称）标记

    struct Case { bool inputMirrored; bool targetFlip; };
    const Case cases[] = {
        {true, false},  // 10：源已镜像、目标不镜像 → 翻
        {true, true },  // 11：源已镜像 + 目标镜像 → XOR 抵消，不翻
    };
    for (const auto& c : cases) {
        cv::Mat input(H, W, CV_8UC3, cv::Scalar(0, 0, 0));
        input(marker).setTo(cv::Scalar(255, 255, 255));

        Engine engine;
        engine.setInputMirroredX(c.inputMirrored);
        engine.setFlip(c.targetFlip, false);
        engine.setRealtimePipeline(makeRtPipeline(std::make_shared<EdgeMarkerDetector>()));
        for (int i = 0; i < 3; ++i) {  // stableFrames=3 达成确认
            engine.processFrameOnce(input);
        }

        const auto tracks = engine.getTrackViews();
        ASSERT_EQ(tracks.size(), 1u)
            << "input=" << c.inputMirrored << " target=" << c.targetFlip;
        // 期望由**契约本身**（input XOR target）与半开区间镜像公式独立算出，**刻意不复用**
        // FrameTransform::mapRect/effectiveFlipX——否则「生产接线被写坏」会与期望一起漂移、互相抵消
        //（判别力归零）。水平镜像：最左输出像素 = W − (x + width)。
        const bool expectFlip = (c.inputMirrored != c.targetFlip);
        const cv::Rect expected = expectFlip
            ? cv::Rect(W - marker.x - marker.width, marker.y, marker.width, marker.height)
            : marker;
        EXPECT_EQ(tracks[0].bbox, expected)
            << "input=" << c.inputMirrored << " target=" << c.targetFlip;
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

// FACE-TRACK-002（生产恒传 detectionRan=true 的行为面）：检测节流模式**不得**让帧循环跳过真实检测。
// 前置断言把「确实处于节流模式」钉住，否则本用例与所测命题无关（节流未启用时当然照常检测）。
// 期望：节流只影响配置与展示，帧循环仍每帧真实调用检测 ⇒ 实际空检测照常累计缺失，已确认轨迹在
// 连续 2 次空检测后消失。
// 判别点：把 detThrottleMode_ / detIntervalMs_ 接进 runRealtimeDetect，在节流窗内返回
//        detectionRan=false ⇒ TrackCoordinator::update 走 `if (!detectionRan) return views();` 早退，
//        旧轨迹永不消失 ⇒ 末条断言（size()==0）翻红。
// 源文本侧的对应守卫见 test_track_coordinator.cpp 的 EngineStructuralGuard 用例。
TEST(EngineTest, DetectionThrottleModeDoesNotSuppressRealDetection) {
    Engine engine;
    engine.updateDetectionThrottle("manual", 10000);  // 10s 间隔：若真接线，本帧必被「跳过」
    ASSERT_NE(engine.getDetectionThrottleMode(), InferenceThrottleMode::Off)
            << "前置：节流模式必须已置为非 Off，否则本用例不覆盖任何东西";

    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    engine.setRealtimePipeline(makeRtPipeline(det));

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);
    ASSERT_EQ(engine.getTrackViews().size(), 1u);

    det->boxes.clear();  // 实际空检测
    engine.processFrameOnce(frame);
    EXPECT_EQ(engine.getTrackViews().size(), 1u) << "缺失 1 次：已确认轨迹保留";
    engine.processFrameOnce(frame);
    EXPECT_EQ(engine.getTrackViews().size(), 0u)
            << "缺失 2 次必须删除：节流模式不得把本帧变成「跳过检测」而让旧框长生";
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

// FACE-CLEAN-020①：引擎层「两个不同认证身份共存且互不串号」。
// 此前本文件通篇只有 alice，TwoToneEmbedder（按对齐脸灰度均值二值化）自引入以来**零引用**
// ⇒ 引擎的多身份**排序**从未被验证过（ConstantEmbedder 对两张脸给同一向量，天然无法区分排序，
// 这正是 TwoToneEmbedder 存在的理由）。
// ★ 数值构造（已核算）——库条目**必须是 L2 归一化的单位向量**，否则本用例不成立：
//   alice = 512 维各 1/√512（模 1）；bob = 前 256 维各 1/16、其余 0（模 1）。
//   Engine 侧经 FaceSearchOptions{} 走默认 assumeL2Normalized=true ⇒ 打分**就是原始点积**
//   （FaceSearch.cpp:99），只有两端都是单位向量时它才等于余弦。初版用「全 1 / 前 256 个 1」
//   时，暗区探针与两个模板的点积**都是 256（打平）**，胜负退化为插入序 ⇒ 暗区脸误配到 alice。
//   cos(alice, bob_probe) = cos(bob, alice_probe) = 0.7071 > identitySimilarity(0.45)
//   ⇒ 两个身份**互为命中**；但自相似 = 1.0 > 0.7071 ⇒ top-1 仍必须是正确的那一个
//   （assignRealtimeIdentities 取 hits[0] 后比阈值）。
// 判别点：把检索结论从「取 top-1」改成「取第一个过阈值的条目」（或反转相似度排序）
//        ⇒ 暗区脸会拿到库中排在前的 alice，`darkId == "bob"` 翻红。
TEST(EngineTest, RealtimeTwoDistinctIdentitiesCoexistWithoutCrossAssignment) {
    std::vector<float> bobTemplate(512, 0.0f);
    for (int i = 0; i < 256; ++i) bobTemplate[static_cast<std::size_t>(i)] = 0.0625f;  // 1/16
    auto index = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    std::vector<FaceSearchEntry> entries = {
        {"alice", std::vector<float>(512, 0.044194174f)},  // 1/√512
        {"bob", bobTemplate},
    };
    ASSERT_TRUE(index->reset(std::move(entries), 512, err)) << err;

    // 亮区（255）⇒ TwoToneEmbedder 给**单位向量** 512 维各 1/√512（= alice 模板方向）；
    // 暗区（50）⇒ 给**单位向量**「前 256 维各 1/16、其余 0」（= bob 模板方向）。见上方 TwoToneEmbedder
    // 类注释：这里**必须是单位向量**——FaceSearchOptions::assumeL2Normalized 默认为 true ⇒ 打分是
    // **原始点积**（FaceSearch.cpp:99）。初版夹具写「全 1 / 前 256 个 1」正是因此让暗区脸误配 alice。
    // 两块框均 160×200 > 112×112，重采样到嵌入输入时不会把背景均值拉过 128 的判定线。
    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));
    const cv::Rect bright(80, 80, 300, 300);
    frame(bright).setTo(cv::Scalar(255, 255, 255));
    const cv::Rect aliceBox(140, 120, 160, 200);  // 完全落在亮区内
    const cv::Rect bobBox(440, 120, 160, 200);    // 落在暗区（两框中心相距 300px，不被 NMS/包含抑制合并）

    Engine engine;
    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(aliceBox), boxOf(bobBox)};
    engine.setRealtimePipeline(
        makeRtPipeline(det, index, /*reload=*/true, std::make_shared<TwoToneEmbedder>()));

    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);  // stableFrames=3

    const auto tracks = engine.getTrackViews();
    ASSERT_EQ(tracks.size(), 2u) << "两个互不重叠的检测框应各建一条轨迹";
    std::string brightId;  // 亮区那条（应为 alice）
    std::string darkId;    // 暗区那条（应为 bob）
    for (const auto& t : tracks) {
        if (t.bbox.x < bright.x + bright.width) {
            brightId = t.stableId;
        } else {
            darkId = t.stableId;
        }
    }
    EXPECT_EQ(brightId, "alice");
    EXPECT_EQ(darkId, "bob") << "暗区脸不得被串到库中排在前的 alice（top-1 排序被破坏）";
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

TEST(EngineTest, RuntimeSwitchDuringDetectionDoesNotCountOldSpaceIdentity) {
    auto index = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    ASSERT_TRUE(index->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, err));
    auto oldDetector = std::make_shared<BlockingDetector>();
    oldDetector->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    Engine engine;
    ASSERT_TRUE(engine.setRealtimePipeline(makeRtPipeline(oldDetector, index, true, nullptr, std::string(64, 'a'))));
    cv::Mat frame(480, 640, CV_8UC3, cv::Scalar(50, 50, 50));
    std::thread inFlight([&] { engine.processFrameOnce(frame); });
    bool entered;
    {
        std::unique_lock<std::mutex> lock(oldDetector->mutex);
        entered = oldDetector->cv.wait_for(lock, std::chrono::seconds(2), [&] { return oldDetector->entered; });
    }
    auto nextDetector = std::make_shared<ScriptedDetector>();
    nextDetector->boxes = oldDetector->boxes;
    const bool switched = engine.setRealtimePipeline(makeRtPipeline(nextDetector, index, true, nullptr, std::string(64, 'b')));
    {
        std::lock_guard<std::mutex> lock(oldDetector->mutex);
        oldDetector->released = true;
    }
    oldDetector->cv.notify_all();
    inFlight.join();
    ASSERT_TRUE(entered);
    ASSERT_TRUE(switched);
    auto tracks = engine.getTrackViews();
    // 首次检测尚未达到几何轨迹三帧确认门槛，允许对外视图为空。
    for (const auto& track : tracks) EXPECT_FALSE(track.identityPending);
    for (int i = 0; i < 2; ++i) ASSERT_TRUE(engine.processFrameOnce(frame));
    ASSERT_EQ(engine.getTrackViews().size(), 1u);
    EXPECT_EQ(engine.getTrackViews()[0].stableId, "Unknown");
    ASSERT_TRUE(engine.processFrameOnce(frame));
    EXPECT_EQ(engine.getTrackViews()[0].stableId, "alice");
}

TEST(EngineTest, SamePipelineReloadedToAnotherSpaceRevokesCommittedIdentity) {
    auto index = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    ASSERT_TRUE(index->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, err));
    auto detector = std::make_shared<ScriptedDetector>();
    detector->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    auto pipeline = makeRtPipeline(detector, index, true, nullptr, std::string(64, 'a'));
    Engine engine;
    ASSERT_TRUE(engine.setRealtimePipeline(pipeline));
    cv::Mat frame(480, 640, CV_8UC3, cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(engine.processFrameOnce(frame));
    ASSERT_EQ(engine.getTrackViews()[0].stableId, "alice");
    ASSERT_TRUE(pipeline->reload(rtManifest(std::string(64, 'b')), "", "arm64-v8a", "").ok);
    ASSERT_TRUE(engine.setRealtimePipeline(pipeline));
    ASSERT_EQ(engine.getTrackViews().size(), 1u);
    EXPECT_EQ(engine.getTrackViews()[0].stableId, "Unknown");
    EXPECT_FALSE(engine.getTrackViews()[0].identityPending);
    for (int i = 0; i < 2; ++i) ASSERT_TRUE(engine.processFrameOnce(frame));
    EXPECT_EQ(engine.getTrackViews()[0].stableId, "Unknown");
    ASSERT_TRUE(engine.processFrameOnce(frame));
    EXPECT_EQ(engine.getTrackViews()[0].stableId, "alice");
}

// ── 第三波审查 #4 残留竞态：合并 API applyGalleryUpdate ──

// 合并 API 语义：applyGalleryUpdate 必须在**单次调用**内同时完成「换图库覆盖 + 精准撤销身份」，
// 两项效果均来自这一次调用，不存在「图库已换但身份未撤」/「身份已撤但图库未换」的可观测中间态。
TEST(EngineTest, GalleryReplacementRejectsAnotherTemplateSpaceWithoutChangingIdentity) {
    const std::string spaceA(64, 'a'), spaceB(64, 'b');
    auto first = std::make_shared<FaceSearchLinearIndex>();
    auto other = std::make_shared<FaceSearchLinearIndex>();
    std::string error;
    ASSERT_TRUE(first->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, error));
    ASSERT_TRUE(other->reset({{"alice", std::vector<float>(512, -1.0f)}}, 512, error));
    auto detector = std::make_shared<ScriptedDetector>();
    detector->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    Engine engine;
    ASSERT_TRUE(engine.setRealtimePipeline(makeRtPipeline(detector, first, true, nullptr, spaceA)));
    ASSERT_TRUE(engine.applyGalleryUpdate(first, {"alice"}, spaceA));
    cv::Mat frame(480, 640, CV_8UC3, cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(engine.processFrameOnce(frame));
    ASSERT_EQ(engine.getTrackViews().size(), 1u);
    ASSERT_EQ(engine.getTrackViews()[0].stableId, "alice");
    EXPECT_FALSE(engine.applyGalleryUpdate(other, {"alice"}, spaceB));
    EXPECT_FALSE(engine.applyGalleryUpdate(other, {"alice"}));
    ASSERT_TRUE(engine.processFrameOnce(frame));
    EXPECT_EQ(engine.getTrackViews()[0].stableId, "alice");
}

TEST(EngineTest, GallerySpaceChangeRevokesSamePersonAndRejectsOldGallery) {
    const std::string spaceA(64, 'a'), spaceB(64, 'b');
    auto gallery = std::make_shared<FaceSearchLinearIndex>();
    std::string error;
    ASSERT_TRUE(gallery->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, error));
    auto detector = std::make_shared<ScriptedDetector>();
    detector->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    Engine engine;
    ASSERT_TRUE(engine.setRealtimePipeline(makeRtPipeline(detector, gallery, true, nullptr, spaceA)));
    cv::Mat frame(480, 640, CV_8UC3, cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(engine.processFrameOnce(frame));
    ASSERT_EQ(engine.getTrackViews()[0].stableId, "alice");
    ASSERT_TRUE(engine.setRealtimePipeline(makeRtPipeline(detector, nullptr, true, nullptr, spaceB)));
    EXPECT_EQ(engine.getTrackViews()[0].stableId, "Unknown");
    EXPECT_FALSE(engine.getTrackViews()[0].identityPending);
    EXPECT_FALSE(engine.applyGalleryUpdate(gallery, {"alice"}, spaceA));
    ASSERT_TRUE(engine.applyGalleryUpdate(gallery, {"alice"}, spaceB));
    ASSERT_TRUE(engine.processFrameOnce(frame));
    EXPECT_EQ(engine.getTrackViews()[0].stableId, "Unknown");
    EXPECT_TRUE(engine.getTrackViews()[0].identityPending);
}

TEST(EngineTest, PerformanceCsvMeasuresActualStagesAndResidentMemory) {
    struct WorkingDirectory {
        std::filesystem::path previous = std::filesystem::current_path();
        std::filesystem::path directory = std::filesystem::temp_directory_path() /
            ("rk_engine_perf_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        WorkingDirectory() {
            std::filesystem::create_directories(directory / "tests/metrics");
            std::filesystem::current_path(directory);
        }
        ~WorkingDirectory() { std::filesystem::current_path(previous); }
    } working;
    {
        Engine engine;
        auto detector = std::make_shared<ScriptedDetector>();
        detector->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
        detector->delayMs = 30;
        engine.setRealtimePipeline(makeRtPipeline(detector));
        engine.setOnResultCallback([](const std::string&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        });
        cv::Mat frame(480, 640, CV_8UC3, cv::Scalar(50, 50, 50));
        ASSERT_TRUE(engine.processFrameOnce(frame, 7.0));
    }
    // 析构已排空Reporter；检查真实Engine输出，防止各字段只在测试里人为填充而生产仍恒为0。
    std::ifstream csv(working.directory / "tests/metrics/engine_perf.csv");
    ASSERT_TRUE(csv.is_open());
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(csv, line)));
    std::replace(line.begin(), line.end(), ',', ' ');
    double decode = 0, pre = 0, infer = 0, post = 0, render = 0;
    long long rss = 0;
    std::istringstream fields(line);
    ASSERT_TRUE(static_cast<bool>(fields >> decode >> pre >> infer >> post >> render >> rss));
    EXPECT_DOUBLE_EQ(decode, 7.0);
    EXPECT_GT(pre, 0.0);
    EXPECT_GE(infer, 25.0);
    EXPECT_GT(post, 0.0);
    EXPECT_GE(render, 25.0);
    EXPECT_GT(rss, 0);
}

TEST(EngineTest, InitializedPerformanceCsvUsesPrivateStorageWithoutPrecreatedMetrics) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("rk_engine_private_perf_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto cascade = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "app/src/main/assets/lbpcascade_frontalface.xml";
    ASSERT_TRUE(std::filesystem::is_regular_file(cascade));
    ASSERT_FALSE(std::filesystem::exists(directory / "metrics"));
    {
        Engine engine;
        ASSERT_TRUE(engine.initialize(-1, cascade.string(), directory.string()));
        ASSERT_TRUE(engine.setRealtimePipeline(makeRtPipeline(std::make_shared<ScriptedDetector>())));
        ASSERT_TRUE(engine.processFrameOnce(cv::Mat(480, 640, CV_8UC3, cv::Scalar(50, 50, 50)), 4.0));
    }
    std::ifstream csv(directory / "metrics/engine_perf.csv");
    ASSERT_TRUE(csv.is_open()) << "正式初始化必须在应用私有存储中创建性能输出目录";
    std::string row;
    ASSERT_TRUE(static_cast<bool>(std::getline(csv, row)));
    EXPECT_EQ(std::count(row.begin(), row.end(), ','), 5);
}

TEST(EngineTest, GalleryUpdateDoesNotReshowTracksHiddenAfterDetectionFailure) {
    auto index = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    ASSERT_TRUE(index->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, err)) << err;
    Engine engine;
    auto detector = std::make_shared<ScriptedDetector>();
    detector->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    engine.setRealtimePipeline(makeRtPipeline(detector, index));
    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(engine.processFrameOnce(frame));
    ASSERT_EQ(engine.getTrackViews().size(), 1u);
    ASSERT_EQ(engine.getTrackViews()[0].stableId, "alice");
    detector->fail = true;
    ASSERT_TRUE(engine.processFrameOnce(frame));
    ASSERT_TRUE(engine.getTrackViews().empty());

    // 检测失败时隐藏轨迹；图库更新只能撤销身份，不能重新展示未被当前帧确认的旧框。
    ASSERT_TRUE(engine.applyGalleryUpdate(index, {"alice"}));
    EXPECT_TRUE(engine.getTrackViews().empty());
}

TEST(EngineTest, ApplyGalleryUpdateRevokesPendingIdentityImmediately) {
    auto index = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    ASSERT_TRUE(index->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, err)) << err;
    Engine engine;
    auto detector = std::make_shared<ScriptedDetector>();
    detector->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    auto empty = std::make_shared<FaceSearchLinearIndex>();
    ASSERT_TRUE(empty->reset({}, 512, err)) << err;
    engine.setRealtimePipeline(makeRtPipeline(detector, empty));
    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));
    // 先确认框，再开始累计身份；框确认与身份确认是两道独立门槛。
    for (int i = 0; i < 3; ++i) engine.processFrameOnce(frame);
    ASSERT_TRUE(engine.applyGalleryUpdate(index, {"alice"}));
    engine.processFrameOnce(frame);
    ASSERT_EQ(engine.getTrackViews().size(), 1u);
    ASSERT_TRUE(engine.getTrackViews()[0].identityPending);
    ASSERT_EQ(engine.getTrackViews()[0].stableId, "Unknown");

    // 换库时尚未确认的候选也属于旧库；必须即时撤销，不能等下一帧修正对外状态。
    ASSERT_TRUE(engine.applyGalleryUpdate(index, {"alice"}));
    EXPECT_TRUE(engine.getTrackViews()[0].identityPending);
    ASSERT_TRUE(engine.applyGalleryUpdate(empty, {}));
    const auto revoked = engine.getTrackViews();
    ASSERT_EQ(revoked.size(), 1u);
    EXPECT_FALSE(revoked[0].identityPending);
    EXPECT_GT(revoked[0].bbox.area(), 0);
}

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

// ★ 行为判别：Engine::noteEmbedError 的限频闸门必须是**纯时间窗**（不分站点、不看文本）。
//
// 背景：闸门已抽为 pipeline::LogRateLimiter，core_unit_tests 有它的单元用例，另有一条读
// Engine.cpp 文本的**形态守卫**（tests/cpp/test_log_rate_limiter.cpp）。但那条只是文本判据——
// 把闸门**回退成**「文本与上一条相同且在窗内才抑制」，全套仍绿。本用例补的正是**行为**承重：
// Engine.cpp 不在 core_unit_tests 的编译面内，故必须落在编 Engine.cpp 的 core_gtest_tests
// （本文件）。noteEmbedError 是 private，经其唯一间接生产者 captureEnrollmentFaces 驱动
//（照 EnrollmentCaptureCallbacksOnlyWhenEnabled 那条路子在无模型环境下走）。
//
// 设计（把判别点从"时刻"改成"条数"，因此**无需**时钟接缝、也**无需**缩短 5s 窗口）：在**同一次**
// processFrameOnce（⇒ 同一条限频窗）内，用 TwoTextErrorEmbedder 让两张脸各产出一条**文本不同**
// 的 err：
//   · 纯时间窗闸门（当前实现）⇒ 窗内只放行一条 ⇒ 该行只出现 **1** 次；
//   · 回退版（文本相同才抑制）⇒ 两条不同文本都放行 ⇒ 出现 **2** 次 ⇒ 确定性 RED。
// 正向锚：必须出现 **≥1** 次——防"一条都没打"这种另一种破法（本仓明确拒绝"静默"）。
TEST(EngineTest, NoteEmbedErrorLimiterSuppressesSecondDistinctTextWithinWindow) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("rk_engine_ratelimit" + uniqueSuffix());
    std::filesystem::create_directories(dir);
    const std::string fileName = "rk_engine_ratelimit.log";

    // 开文件 sink（**进程级**，用完必须关，勿污染其它用例）。与 test_native_log_redaction.cpp
    // 同范式：setLogDirs(dir,"",fileName) 开、跑完 setLogDirs("","","rk3288.log") 关 FD 并复位
    // 默认文件名（第三参非空；NativeLog 仅在其非空时覆盖默认名，故此处正是「关 FD + 复位默认名」），
    // 再读回文件。
    rklog::setLogDirs(dir.string(), "", fileName);

    auto det = std::make_shared<ScriptedDetector>();
    // 两张**互不重叠**的脸（IoU=0，NMS 与包含框抑制都不会删其一）⇒ captureEnrollmentFaces 逐脸
    // 处理两次。关键点均落在 640×480 画面内（即便关键点仿射失败也会回落到 bbox 裁剪，仍能对齐）。
    RealtimeFaceBox fa = boxOf(cv::Rect(80, 120, 140, 180), 0.9f);
    fa.hasKeypoints = true;
    fa.keypoints = {110.f, 150.f, 190.f, 150.f, 150.f, 190.f, 125.f, 225.f, 175.f, 225.f};
    RealtimeFaceBox fb = boxOf(cv::Rect(400, 120, 140, 180), 0.9f);
    fb.hasKeypoints = true;
    fb.keypoints = {430.f, 150.f, 510.f, 150.f, 470.f, 190.f, 445.f, 225.f, 495.f, 225.f};
    det->boxes = {fa, fb};

    auto emb = std::make_shared<TwoTextErrorEmbedder>();
    // gallery = nullptr ⇒ assignRealtimeIdentities 在读库处早退（不产 err），限频窗只被
    // captureEnrollmentFaces 的两条 err 使用，判别不掺入识别侧噪声。
    Engine engine;
    engine.setRealtimePipeline(makeRtPipeline(det, nullptr, /*reload=*/true, emb));
    engine.setEnrollmentCapture(true);  // captureEnrollmentFaces 仅在采集模式开启时被调用

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(90, 90, 90));
    engine.processFrameOnce(frame);

    rklog::setLogDirs("", "", "rk3288.log");  // 关闭 FD 并复位默认文件名

    const std::string content = readFileAll(dir / fileName);
    std::filesystem::remove_all(dir);

    // 前置自检：两张脸都被嵌入（⇒ 两条 err 都确实被生产出来）。若此断言失败，说明场景未构造成功
    // （例如脸被后处理删了一张），后续计数断言便无意义——用 ASSERT 明确判失败，而非静默假通过。
    ASSERT_EQ(emb->calls.load(), 2)
        << "场景未构造：captureEnrollmentFaces 未逐脸调用 embed 两次（脸被后处理删了一张？）";

    const int total = countOccurrences(content, "人脸特征路径失败 err=");
    const int alpha = countOccurrences(content, "err=vitest-embed-alpha");          // embed 失败站点
    const int dim = countOccurrences(content, "维度不符: 实际 256 期望 512");        // 维度不符站点

    // ③ 正向锚：至少落一条（拒绝"静默"）。缺了这条，"闸门恒抑制/日志 sink 未生效"会伪装成通过。
    EXPECT_GE(total, 1)
        << "noteEmbedError 一条都没打：日志 sink 未生效或闸门被改成恒抑制（禁止静默）";
    // 两条已知文案恰好命中一条：证明断言针对**真实文案**（且两条文本确实不同），而非臆造串。
    EXPECT_EQ(alpha + dim, 1)
        << "期望恰好命中一条已知 err 文本（vitest-embed-alpha / 维度不符: 实际 256 期望 512）";
    // ★ 判别点：同一时间窗内两条**文本不同**的 err 只应放行一条。
    //   把闸门回退成「文本与上一条相同且在窗内才抑制」⇒ 两条不同文本都放行 ⇒ total==2 ⇒ 本行翻红。
    EXPECT_EQ(total, 1)
        << "同一限频窗内两条不同文本的 err 只应落 1 条；出现 2 条说明闸门退化成了"
           "「文本相同才抑制」（多张脸给出不同 err 文本时会逐帧×每脸刷屏、日志量无上界）";
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

    // FACE-PIPE-017：校验② 在**绘制之后**命中，若不同步重绘，发布帧仍是校验① 之前画上的认证**绿框**，
    // 而视图层已变 Unknown——同帧「绿框 + AUTH_FAIL Unknown」自相矛盾。
    // **取样必须发生在下面「再次驱动」之前**：此刻 renderFrame_ 正是 framer 那一帧（校验② 窗口内的发布帧，
    // 尚未被覆盖）；若把本断言放到那次驱动之后，取到的会是「撤销后重新认脸」的合法红框帧，断言恒真、放过
    // 缺陷。断言框顶边像素：红框 R≈255 / 绿框 G≈255，背景 (50,50,50)。
    {
        const auto preRedrive = engine.getTrackViews();
        ASSERT_EQ(preRedrive.size(), 1u);
        cv::Mat published;
        ASSERT_TRUE(latestRender(engine, published));
        const int sx = preRedrive[0].bbox.x + preRedrive[0].bbox.width / 2;
        const int sy = preRedrive[0].bbox.y;  // 顶边：红框 R 通道 ≈255，绿框 G 通道 ≈255
        ASSERT_LT(sy, published.rows);
        ASSERT_LT(sx, published.cols);
        const cv::Vec3b border = published.at<cv::Vec3b>(sy, sx);
        EXPECT_GT(static_cast<int>(border[2]), 200)
                << "发布帧像素与视图层不自相矛盾：门内校验②命中后必须同步重绘为未认证红框";
        EXPECT_LT(static_cast<int>(border[1]), 100);
    }

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
// ⚠️ **覆盖现状（别照旧注释以为只有代表入口被覆盖）**：
//   · setRealtimePipeline / applyGalleryUpdate / getTrackViews / processFrameOnce 四个入口各有
//     **专属**判别探针，见下方 EmitGateReentryRejectedOnAllFourRemainingEntryPoints；
//     本用例只保留最早的那条代表（applyGalleryUpdate，含「被拒绝的调用不得产生副作用」这一额外断言）。
//   · `run()` 的守卫**仍无行为用例**（子代理3 用探针实测确认：注释掉 `Engine::run()` 顶部那行守卫，
//     当时的套件仍全绿）。原因不是「做不到」，而是**失败形态不可诊断**：在钩子（已持 identityEmitMu_）
//     内调用 run() 时，守卫缺失会让它经 processFrameRealtime 取 tracksMu_ 后在 renderResults 再取
//     同一把 identityEmitMu_ ⇒ **同线程自锁**（挂死也算红，但 CI 表现为超时而非干净失败）。
//     故 run() 目前**只**有源文本结构断言兜底（test_track_coordinator.cpp 的
//     EngineStructuralGuard 用例），其脆弱性（改写法即可绕过）已在那边如实登记。
//
// 若要给 run() 补**行为**用例，子代理4 给出了两条安全且确定的测法（仅为构造性论证，未实做）：
//   · 设计 A（需一个极小的测试接缝）：在 `run()` 的守卫之后、`isRunning_ = true` **之前**加一个
//     `RK_ENABLE_TEST_SEAMS` 下的虚接缝（与既有 onBeforeRenderPublish / onBeforePublishLocked 同款），
//     测试子类覆写它并抛哨兵异常。无守卫时该异常在**循环外的无 try 区**抛出 → 穿透到 worker 的
//     `catch(...)` → `run()` **根本不进循环** ⇒ 无自旋、无 detach、无泄漏，断言「接缝未被触发」即可判别。
//     代价 = 1 个虚函数 + 1 行调用（生产不定义宏 ⇒ 零成本）。
//   · 设计 B（零生产改动）：让 `run()` 无帧可消费（不接相机）⇒ 恒走 `processed=false → sleep → continue`，
//     永不触碰 identityEmitMu_；另起守护线程限时调 `stop()`（只置原子、不取两把锁）以有界终止，再用
//     gtest `CaptureStdout()` 判「守卫之后那行 `Engine started.` 是否输出」——**文本断言，非计时断言**。
//   ⇒ 仍**不做**的理由：该守卫**生产不可达**（结果回调不可能同步调用阻塞主循环），收益不抵成本。
//     **若该路径将来变得可达，按设计 A 补测。**
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

// FACE-PIPE-015 免死锁契约的**逐入口**判别探针：setRealtimePipeline / applyGalleryUpdate /
// getTrackViews / processFrameOnce 四个入口各一条断言（run() 不用行为用例，理由见上方注释块）。
//
// 为什么同一线程内能**确定性**触发：重入标记是 thread_local，只有**引擎线程自己**能命中；而
// onBeforePublishLocked（Engine.h 的测试接缝）正运行在引擎线程、且**已在发射门内**
//（renderResults 先持 identityEmitMu_、再经 RAII 置位标记，随后调用本钩子——见 Engine.h 对
// identityEmitMu_ 的条目）。故把重入调用写进本钩子即是确定性的复现，不需要 sleep/竞态。
//
// 四个探针的判别力（逐一登记「改坏哪一处会让它变红」）：
//   · setRealtimePipeline(nullptr) → false：**只可能**来自本拒绝（无别的 false 出口）；
//   · applyGalleryUpdate(空库快照, {}) → false：同上（Engine.h 明确记载这两者的 false 无歧义）；
//     两者若守卫缺失，都会在取到 tracksMu_ 后**等待已被本线程持有的 identityEmitMu_**
//     ⇒ 同线程自锁（MSVC 抛 system_error 被 worker 吞掉 ⇒ probeDone 不置位 ⇒ 本用例干净失败）。
//   · getTrackViews().size() == 0：与「确实无已确认轨迹」同值 ⇒ 前置**必须**已建立 ≥1 条已确认轨迹
//     （见用例断言的 stableId == "alice"），否则本探针恒真、不判别；
//   · processFrameOnce(合法帧) → false：与「preprocessFrame 失败」同值 ⇒ 必须传入一张**本来能成功**
//     的帧；用例在探针结束后另驱动一帧断言 true 作为**正对照**，证明该帧合法、故而那个 false 只能
//     来自拒绝。
// ⚠️ 探针顺序不可换：processFrameOnce 必须**最先**（setRealtimePipeline(nullptr) 一旦被接受就会关掉
//    实时管线，其后驱动帧会因模型缺失返回 false，判别力归零；当前它理应被拒绝，故顺序安全）。
// ⚠️ 防递归：processFrameOnce 内部会再进 renderResults → 再次调用本钩子；故调用前先清 probeArmed。
class EmitGateProbeEngine : public Engine {
public:
    std::atomic<bool> probeArmed{false};
    std::atomic<bool> probeDone{false};

    std::atomic<bool> frameOk{true};          // 期望 false
    std::atomic<bool> setPipelineOk{true};    // 期望 false
    std::atomic<bool> galleryOk{true};        // 期望 false
    std::atomic<std::size_t> viewsSize{999};  // 期望 0

    std::shared_ptr<const FaceSearchLinearIndex> gallery;  // 探针内换入的库（调用应被拒绝，内容无关）
    cv::Mat probeFrame;                                    // 探针内驱动帧用的合法帧

protected:
    void onBeforePublishLocked() override {
        if (!probeArmed.load()) return;  // 其余帧走等价空钩子
        probeArmed.store(false);         // 先清，避免内层 renderResults 递归回入本钩子
        frameOk.store(processFrameOnce(probeFrame));
        setPipelineOk.store(setRealtimePipeline(nullptr));
        galleryOk.store(applyGalleryUpdate(gallery, {}));
        viewsSize.store(getTrackViews().size());
        probeDone.store(true);
    }
};

TEST(EngineTest, EmitGateReentryRejectedOnAllFourRemainingEntryPoints) {
    auto aliceIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string err;
    ASSERT_TRUE(aliceIdx->reset({{"alice", std::vector<float>(512, 1.0f)}}, 512, err)) << err;
    auto emptyIdx = std::make_shared<FaceSearchLinearIndex>();
    std::string e2;
    ASSERT_TRUE(emptyIdx->reset({}, 512, e2)) << e2;

    // 堆分配：RED（未拒绝）时 worker 会卡在自锁上，需 detach 并**泄漏**本对象以保证套件存活。
    EmitGateProbeEngine* engine = new EmitGateProbeEngine();

    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    ASSERT_TRUE(engine->setRealtimePipeline(makeRtPipeline(det, aliceIdx)));

    const cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH, CV_8UC3,
                        cv::Scalar(50, 50, 50));
    engine->probeFrame = frame;
    engine->gallery = emptyIdx;

    for (int i = 0; i < 3; ++i) engine->processFrameOnce(frame);  // 确认 alice（stableFrames=3）
    const auto pre = engine->getTrackViews();
    ASSERT_EQ(pre.size(), 1u) << "前置：getTrackViews 探针需要至少一条已确认轨迹才有判别力";
    ASSERT_EQ(pre[0].stableId, "alice");

    engine->probeArmed.store(true);

    // 有界驱动（同上方代表用例的取舍）：探针在 worker 线程内执行，主线程限时等待。
    // ⚠️ 超时分支会 detach，detach 后 worker 仍可能恢复运行，故捕获的一切都不得是本栈局部量：
    //   · workerDone 堆分配 + shared_ptr 按值捕获；frame 按值捕获（cv::Mat 引用计数）；
    //   · engine 是堆指针，超时分支**刻意泄漏**（其内存仍被 detach 线程引用）。
    auto workerDone = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([engine, frame, workerDone] {
        // 吞掉逃逸异常：RED 时同一线程二次取 identityEmitMu_/tracksMu_，MSVC 的 std::mutex 会**抛**
        // system_error（libstdc++ 上真挂死）；任其逃出 worker 线程 → std::terminate 中止进程，
        // 而非「用例失败」。捕获后照常置 workerDone，使 RED 收敛为确定性断言失败。
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
        ADD_FAILURE() << "重入未被拒绝：worker 在超时内未返回（疑似发射门自锁）";
        worker.detach();
        return;  // 泄漏 engine（不得析构：其内存仍被 detach 线程引用）
    }
    worker.join();

    EXPECT_TRUE(engine->probeDone.load());
    EXPECT_FALSE(engine->frameOk.load())
            << "processFrameOnce 的重入守卫缺失（同一帧在门外本应处理成功，见下方正对照）";
    EXPECT_FALSE(engine->setPipelineOk.load()) << "setRealtimePipeline 的重入守卫缺失";
    EXPECT_FALSE(engine->galleryOk.load()) << "applyGalleryUpdate 的重入守卫缺失";
    EXPECT_EQ(engine->viewsSize.load(), 0u) << "getTrackViews 的重入守卫缺失（应返回空快照）";

    // 正对照 + 无副作用：四项均被拒绝 ⇒ 管线仍在、图库未换、身份未撤，故同一张帧在门外照常成功。
    // 这同时证明上面那个 false **只能**来自拒绝，而不是「帧非法 / 预处理失败」。
    EXPECT_TRUE(engine->processFrameOnce(frame));
    const auto post = engine->getTrackViews();
    ASSERT_EQ(post.size(), 1u);
    EXPECT_EQ(post[0].stableId, "alice");

    delete engine;
}

// ── P8/D8：run() 同线程重入拒绝的**行为**测试 ──────────────────────────────────
// 此前 run() 只有**源文本结构断言**（test_track_coordinator.cpp 的
// EngineStructuralGuard.AllTracksMuEntryPointsCallEmitGateReentryGuard）而无行为用例；原因是
// 失败形态不可诊断（守卫缺失 ⇒ 进入阻塞主循环/同线程自锁 ⇒ 挂死，CI 表现为超时）。
// 本用例补上行为面，并用**独立 CTest + 超时**兜住失败形态：核心套件默认不跑它（DISABLED_ 前缀），
// 仅由 CTest `engine_run_reentry_behavior`（--gtest_also_run_disabled_tests + 单项过滤）启用；
// 超时只终止**子进程**，**不给生产加看门狗**。
//
// 机制：run() 顶部有 `if (reenteredFromIdentityEmitGate(__func__)) return;`；而
// onBeforePublishLocked 本身就跑在引擎线程、且**已持 identityEmitMu_**（renderResults 发射临界区内）。
// 故在同一线程内、发射门内同步调用 run()，守卫生效时应**立即返回**；守卫缺失时会进入阻塞主循环
//（帧源无帧 ⇒ 恒 sleep）⇒ 由 CTest TIMEOUT 判失败。
class RunReentryProbeEngine : public Engine {
public:
    std::atomic<bool> armed{false};     // 仅对受测帧启用
    std::atomic<bool> entered{false};   // 钩子已在发射门内执行
    std::atomic<bool> returned{false};  // run() 已返回（守卫生效）

protected:
    void onBeforePublishLocked() override {
        if (!armed.load()) return;      // 其余帧走等价空钩子
        armed.store(false);             // 先解除武装：即便 run() 真进入循环，也不会再触发本钩子
        entered.store(true);
        run();                          // 同线程、已持 identityEmitMu_：应被守卫立即拒绝并返回
        returned.store(true);
    }
};

// 注意：本用例**故意禁用**（DISABLED_ 前缀），只在独立 CTest 下运行（见上方说明与 CMakeLists.txt）。
TEST(EngineTest, DISABLED_RunReentryFromEmitGateReturnsImmediately) {
    // 堆分配：RED（守卫缺失）时会进入阻塞主循环，由 CTest TIMEOUT 终止整个子进程；
    // 走不到 delete，避免“等待一个永不返回的线程”把失败形态从超时变成别的东西。
    // 静态类型须为派生类（探针成员在派生类上）。
    RunReentryProbeEngine* engine = new RunReentryProbeEngine();
    auto det = std::make_shared<ScriptedDetector>();
    det->boxes = {boxOf(cv::Rect(200, 150, 160, 200))};
    ASSERT_TRUE(engine->setRealtimePipeline(makeRtPipeline(det)));

    cv::Mat frame(rk_core::config::FRAME_HEIGHT, rk_core::config::FRAME_WIDTH,
                  CV_8UC3, cv::Scalar(50, 50, 50));
    engine->armed.store(true);  // 武装：下一帧的发射临界区内将同步调用 run()
    // 驱动足够合法帧，确保发布路径确实进入发射临界区（钩子只在发射门内被调用）。
    for (int i = 0; i < 3 && !engine->entered.load(); ++i) {
        engine->processFrameOnce(frame);
    }

    // 前置锚：钩子必须真的被执行——否则本用例在“没进钩子”时空转假绿。
    EXPECT_TRUE(engine->entered.load())
            << "钩子未被执行：发布路径未进入发射临界区，本用例未覆盖任何东西";
    EXPECT_TRUE(engine->returned.load())
            << "run() 未被守卫拒绝：从发射门内同线程重入 run() 应立即可返回（RED 时会进入阻塞循环）";

    delete engine;  // 守卫生效时安全析构（run() 未置 isRunning_、未进入循环）
}

// ── §10.11 D4b：平台默认值的正向覆盖（本 target **无** RK_BENCH_ENFORCE_BUDGET）──
//
// 本 target（core_gtest_tests）经 RK_FACE_INFER_CORE_SOURCES 编译 RealtimeFacePipeline.cpp，
// 却**不定义** RK_BENCH_ENFORCE_BUDGET（该宏仅当全局选项显式设置时才注入——见 CMakeLists.txt 中
// core_gtest_tests 的 `if(NOT RK_BENCH_ENFORCE_BUDGET STREQUAL "")` 注入块；
// 本仓选项默认留空 ⇒ 零行为变化）⇒ .cpp 的 kDefaultEnforceBudget 走**平台回落分支**
// （无该宏 ⇒ 取该常量的 `#else` 分支 = `rkDefaultEnforceBudgetForArch(kIsAarch64Target)`；x64 下
// `kIsAarch64Target=false`），即生产 x64 的真实默认。
//
// 这正是 core_unit_tests 那条用例**覆盖不到**的：core_unit_tests 被注入了该宏，其 .cpp 走 `#if`
// 分支，`#else` 根本不被编译。故「非 arm64 平台默认 = false」这条语义的唯一断言点在此。
TEST(RealtimeFacePipelinePlatformDefault, DefaultEnforceBudgetMatchesPlatform) {
    // FACE-TEST-002（如实口径）：把「平台默认」判定收敛为**不依赖 #if 的纯函数**后，其**映射语义**
    // 在本 x64 target 上可断言——**本用例内共 3 条断言**会随该函数变异而翻红：下面的两条映射断言
    // （rkDefaultEnforceBudgetForArch(true/false)），**加上**更下方 `EXPECT_EQ(pipe.benchmarkConfig().enforceBudget,
    // kExpectedPlatformDefault)`——后者经默认构造 → RealtimeFacePipeline.cpp 的
    // `kDefaultEnforceBudget = rkDefaultEnforceBudgetForArch(kIsAarch64Target)` → `bench_.enforceBudget`，与同一函数耦合。
    // 整用例真实红数：`return true;` → **2**、`return !isAarch64;` → **3**、`return false;` → **1**（并**非**「单侧写死必恰好一条」）。
    // 下方对 benchmarkConfig().enforceBudget 的断言亦覆盖 x64 回落分支经构造函数写入 bench_ 的实取值。
    // ⚠️ 但 .cpp 里 `#if defined(__aarch64__)` 的 **arm64 选臂在 x64 上仍是死代码**（无测试可及）——
    // 该缺口仍未消除；要真正闭合需 arm64 或交叉编译目标。
    // 生产侧的 kDefaultEnforceBudget 经 rkDefaultEnforceBudgetForArch(isAarch64) 取值，
    // 故把该函数改坏亦会令 `benchmarkConfig().enforceBudget` 随之变（耦合链见上方注释）。
    EXPECT_TRUE(rkDefaultEnforceBudgetForArch(true))
        << "arm64（RK3288）平台默认必须为 true（强制预算）；把该分支改坏即令本断言红";
    EXPECT_FALSE(rkDefaultEnforceBudgetForArch(false))
        << "非 arm64（x64 等）平台默认必须为 false（仅测量）；把该分支改坏即令本断言红";

    // 期望值按**平台回落分支**逐字写出，与 RealtimeFacePipeline.cpp 的 kDefaultEnforceBudget 分支
    // 一一对应。此处的期望值**不得**由 RK_BENCH_ENFORCE_BUDGET 推导——在默认配置下本 target 无该宏，
    // 推导出来会是空/false，等于把被测对象当成期望值。
#if defined(RK_BENCH_ENFORCE_BUDGET)
    // 显式设置构建期选项时，「平台默认」被该选项整体覆盖 ⇒ 「平台回落分支的取值」这一命题在本配置下
    // 不适用。如实跳过而非按平台推导（否则会假红）。这是「用构建期选项验证 ARM64 语义」时的已知边界。
    GTEST_SKIP() << "RK_BENCH_ENFORCE_BUDGET 被显式设置：平台回落分支已被构建期选项覆盖，本用例不适用";
#else
#if defined(__aarch64__)
    constexpr bool kExpectedPlatformDefault = true;   // ARM64（RK3288）：默认强制预算
#else
    constexpr bool kExpectedPlatformDefault = false;  // 其它平台：仅测量、不强判
#endif
    // 默认构造（不调用 setBenchmarkConfig）：bench_ 只能来自构造函数写入的 kDefaultEnforceBudget。
    RealtimeFacePipeline pipe;
    EXPECT_EQ(pipe.benchmarkConfig().enforceBudget, kExpectedPlatformDefault)
        << "构造函数必须把平台默认 kDefaultEnforceBudget 写入 bench_；"
           "在 x64 上把 RealtimeFacePipeline.cpp 的 #else 改成 true 即令本断言翻红";
#endif
}

namespace {
class AccelEnvironmentScope {
public:
    explicit AccelEnvironmentScope(const char* key, const char* value) : key_(key) {
        const char* previous = std::getenv(key);
        hadValue_ = previous != nullptr;
        if (previous) previous_ = previous;
        set(value);
    }
    ~AccelEnvironmentScope() { set(hadValue_ ? previous_.c_str() : nullptr); }
private:
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(key_.c_str(), value ? value : "");
#else
        if (value) setenv(key_.c_str(), value, 1); else unsetenv(key_.c_str());
#endif
    }
    std::string key_, previous_;
    bool hadValue_ = false;
};

rk_accel::AccelContractStatus accelStatus(const Engine& engine, const std::string& key) {
    for (const auto& status : engine.accelerationStatuses()) if (status.key == key) return status;
    ADD_FAILURE() << "缺少加速状态: " << key;
    return {};
}

class NamedNcnnDetector : public ScriptedDetector {
    const char* name() const override { return "scrfd_ncnn"; }
};
class NamedNcnnEmbedder : public ConstantEmbedder {
    const char* name() const override { return "ncnn"; }
};
}

TEST(EngineAccelerationEvidence, CompiledNcnnWithoutLoadedPipelineIsNotEffective) {
    Engine engine;
    EXPECT_FALSE(accelStatus(engine, "ncnn").effective);
    ASSERT_TRUE(engine.setRealtimePipeline(makeRtPipeline(std::make_shared<ScriptedDetector>())));
    EXPECT_FALSE(accelStatus(engine, "ncnn").effective) << "其他后端的快照不能充当 ncnn 证据";
}

TEST(EngineAccelerationEvidence, LoadedNcnnSnapshotRefreshesAndClearingRevokesEvidence) {
    Engine engine;
    auto pipeline = makeRtPipeline(std::make_shared<NamedNcnnDetector>(), nullptr, false,
                                   std::make_shared<NamedNcnnEmbedder>());
    ASSERT_TRUE(engine.setRealtimePipeline(pipeline));
    EXPECT_FALSE(accelStatus(engine, "ncnn").effective);
    ASSERT_TRUE(pipeline->reload(rtManifest(), "", "arm64-v8a", "").ok);
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    EXPECT_TRUE(accelStatus(engine, "ncnn").effective) << "getter 必须读取同对象的新快照";
#else
    EXPECT_FALSE(accelStatus(engine, "ncnn").effective);
#endif
    ASSERT_TRUE(engine.setRealtimePipeline(nullptr));
    EXPECT_FALSE(accelStatus(engine, "ncnn").effective);
}

TEST(EngineAccelerationEvidence, RequestedBackendsWithoutRuntimeEvidenceStayIneffective) {
    AccelEnvironmentScope qualcomm("RK_USE_QUALCOMM", "1");
    AccelEnvironmentScope mpp("RK_USE_MPP", "1");
    AccelEnvironmentScope libyuv("RK_USE_LIBYUV", "1");
    Engine engine;
    for (const auto& key : {"qualcomm", "mpp", "libyuv"}) {
        const auto status = accelStatus(engine, key);
        EXPECT_TRUE(status.requested) << key;
        EXPECT_FALSE(status.effective) << key;
        EXPECT_NE(status.reason, "ok") << key;
    }
}

#if defined(RK_ACCEL_TEST_LIBYUV_STUB)
// 独立验证目标用可控返回码替代本机未安装的 libyuv，仅验证 Engine 的成功/回落接线。
void rkTestSetLibyuvResult(int value);
TEST(EngineAccelerationEvidence, LibyuvSuccessRefreshesEvidenceAndFallbackRevokesIt) {
    AccelEnvironmentScope requested("RK_USE_LIBYUV", "1");
    Engine engine;
    ASSERT_TRUE(engine.setRealtimePipeline(makeRtPipeline(std::make_shared<ScriptedDetector>())));
    engine.setExternalInputEnabled(true);
    struct RunningEngine {
        Engine& engine;
        std::thread worker;
        explicit RunningEngine(Engine& value) : engine(value), worker([&value] { value.run(); }) {}
        ~RunningEngine() { engine.stop(); worker.join(); }
    } running(engine);
    uint64_t sequence = 0;
    auto feed = [&](ExternalFrameFormat format) {
        ExternalFrame input;
        input.format = format;
        input.width = input.height = 4;
        if (format == ExternalFrameFormat::NV21) {
            input.nv21.assign(24, 128);
            input.nv21RowStrideY = 4;
        } else {
            input.y = {std::vector<uint8_t>(16, 128), 4, 1};
            input.u = {std::vector<uint8_t>(4, 128), 2, 1};
            input.v = {std::vector<uint8_t>(4, 128), 2, 1};
        }
        if (!engine.pushExternalFrame(std::move(input))) return false;
        cv::Mat rendered;
        const auto until = std::chrono::steady_clock::now() + kInterleaveWait;
        while (std::chrono::steady_clock::now() < until) {
            if (engine.getRenderFrame(rendered, sequence)) return true;
            std::this_thread::yield();
        }
        return false;
    };
    EXPECT_FALSE(accelStatus(engine, "libyuv").effective);
    for (const auto format : {ExternalFrameFormat::NV21, ExternalFrameFormat::YUV_420_888}) {
        rkTestSetLibyuvResult(0);
        ASSERT_TRUE(feed(format));
        EXPECT_TRUE(accelStatus(engine, "libyuv").effective);
        rkTestSetLibyuvResult(-1);
        ASSERT_TRUE(feed(format));
        EXPECT_FALSE(accelStatus(engine, "libyuv").effective) << "OpenCV 回落不能遗留上帧成功证据";
    }
    rkTestSetLibyuvResult(0);
}
#endif

