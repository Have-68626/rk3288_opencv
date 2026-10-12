#include "FaceInferencePipeline.h"

using namespace rk_core;

#include "FaceDetector.h"
#include "FaceInferStages.h"
#include "FaceProfileRequest.h"
#include "FaceRuntimeProfile.h"
#include "FaceTemplate.h"
#include "ModelRegistry.h"
#include "adapters/ArcFaceAdapter.h"
#include "adapters/ScrfdNcnnAdapter.h"
#include "adapters/YoloFaceAdapter.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

static bool writeAllBytes(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(p, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

static std::filesystem::path makeTempDir(const std::string& suffix) {
    const auto root = std::filesystem::temp_directory_path() / "rk3288_opencv_face_infer_unit_tests";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);

    const auto dir = root / suffix;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

static std::filesystem::path writeTestImage(const std::filesystem::path& dir, const std::string& name) {
    const auto path = dir / name;
    cv::Mat img(64, 64, CV_8UC3, cv::Scalar(10, 20, 30));
    cv::circle(img, cv::Point(32, 32), 12, cv::Scalar(200, 200, 200), -1);
    cv::imwrite(path.string(), img);
    return path;
}

static bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

// —— B06-008 可判别并发 key 切换：A 命中缓存 K1 并在 detect 中阻塞，B 切到 K2，
//    断言使用中的 K1 实例不被销毁（调用期局部共享所有权）。——

static std::mutex g_detMu;
static std::condition_variable g_detCv;
static std::atomic<int> g_detEntered{0};
static std::atomic<bool> g_detRelease{false};
static std::atomic<bool> g_k1Destroyed{false};

struct FakeDetector final : public FaceDetector {
    std::string path;
    ~FakeDetector() override {
        if (path == "/fake/K1.onnx") g_k1Destroyed.store(true);
    }
    bool load(const std::string& modelPath, std::string&) override { path = modelPath; return true; }
    FaceDetections detect(const cv::Mat&, std::string&) override {
        g_detEntered.fetch_add(1);
        g_detCv.notify_all();
        std::unique_lock<std::mutex> lk(g_detMu);
        g_detCv.wait(lk, [] { return g_detRelease.load(); });
        return {};
    }
    const char* name() const override { return "FakeDetector"; }
};

// —— INT8 回落链的生产/消费可达性 + ArcFace 分支节点名注入 ——
// 背景：`FaceInferRequest::int8Enabled` 此前**全仓无任何调用点置真**（该链恒不可达）。生产者落地后
// （`rk3288_cli --int8 1|0`，见 src/cpp/main.cpp），本用例在流水线层面钉住两件事：
//   (a) 该开关确实是链条的**开关**：置 true 才会去 `createEmbedder("arcface_int8")`；
//   (b) `arcface_int8` 分支拿到的是**注入后的 `in0`/`out0`** 而不是 `ArcFaceEmbedderConfig` 的
//       默认 `data`/`output`（本仓两个 INT8 ncnn 模型的真实节点名；默认名走 ncnn 必在
//       `ex.input()` 处按返回码失败 ⇒ 该支被跳过、优先级反转；只多一行 warn，不改变返回值）。
// 判别力来源：捕获发生在**替身适配器的 load() 内**，而非断言实现细节。
//   · 若注入回退成默认 ⇒ 读到 `data`/`output`，断言 (b) 红；
//   · 若注入被挪到 load() 之后（契约违反） ⇒ 同样读到默认值，断言 (b) 红；
//   · 若 `int8Enabled` 不再被消费 ⇒ 替身 load() 永不被调用，`g_int8LoadCalls == 0`，断言 (a) 红。
// 不依赖任何真实模型：「开」侧路径（`.param`）不存在 ⇒ 基类 `ArcFaceAdapter::load` 经
// `ArcFaceEmbedder::initialize` 的 ncnn 分支失败并**返回 false**，两种构建下都不是异常 / 崩溃 ——
// 未启用 ncnn（`RK_HAVE_NCNN=0`）时由 `ArcFaceEmbedder::initialize` 的 Ncnn 分支直接以
// 「当前构建未启用 ncnn」返回 false；启用 ncnn 时先过 `validateNcnnParamFile`（打不开文件即返回
// false），都到不了 `ex.input()`。
// 本用例只读注入进去的节点名。（对照侧 `.onnx` 走 OpenCvDnn：`readNet` 的 cv::Exception 已被
// `ArcFaceEmbedder::initOpenCvDnn` 捕获 ⇒ 同样只是返回 false。）
static std::mutex g_int8Mu;
static int g_int8LoadCalls = 0;
static std::string g_int8InputName;
static std::string g_int8OutputName;

struct NodeNameCapturingArcFaceAdapter final : public ArcFaceAdapter {
    bool load(const std::string& modelPath, std::string& err) override {
        {
            // `inputNodeName()` 返回的是内部引用，必须**拷成值**后带出对象生命周期（A2 审计 M2）。
            std::lock_guard<std::mutex> lk(g_int8Mu);
            g_int8LoadCalls++;
            g_int8InputName = inputNodeName();
            g_int8OutputName = outputNodeName();
        }
        return ArcFaceAdapter::load(modelPath, err);
    }
};

// 「加载成功但输出维度不符」的 INT8 替身（FACE-PIPE-INT8-DIM 用）。
// 刻意**不**碰真实文件：`load` 直接返回 true（模拟加载成功），`embed` 返回 256 维（≠512）。
static std::atomic<int> g_wrongDimEmbedCalls{0};

struct WrongDimInt8Adapter final : public ArcFaceAdapter {
    bool load(const std::string&, std::string&) override { return true; }
    std::optional<std::vector<float>> embed(const cv::Mat&, std::string&) override {
        g_wrongDimEmbedCalls.fetch_add(1);
        return std::vector<float>(256, 0.5f);
    }
};

}  // namespace

bool test_face_infer_image_load_failure() {
    FaceInferRequest req;
    req.imagePath = "this_file_should_not_exist__rk_face_infer__test.jpg";
    req.fakeDetect = true;
    req.fakeEmbedding = true;

    const auto out = runFaceInferOnce(req);
    if (out.ok) return false;
    if (out.stage != "image_load") return false;
    if (out.errorCode != 100) return false;

    if (!contains(out.json, "\"ok\":false")) return false;
    if (!contains(out.json, "\"stage\":\"image_load\"")) return false;
    if (!contains(out.json, "\"frame\":{")) return false;
    if (!contains(out.json, "\"w\":0")) return false;
    if (!contains(out.json, "\"h\":0")) return false;
    if (!contains(out.json, "\"metrics\":{")) return false;
    if (!contains(out.json, "\"msLoad\":")) return false;
    if (!contains(out.json, "\"msTotal\":")) return false;
    return true;
}

bool test_face_infer_yolo_load_failure() {
    const auto dir = makeTempDir("yolo_load_failure");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    FaceInferRequest req;
    req.imagePath = imgPath.string();
    req.fakeDetect = false;
    req.fakeEmbedding = true;
    req.yoloBackend = "opencv";
    req.yoloModelPath = "";

    const auto out = runFaceInferOnce(req);
    if (out.ok) return false;
    if (out.stage != "yolo_load") return false;
    if (out.errorCode != 200) return false;
    if (!contains(out.json, "\"stage\":\"yolo_load\"")) return false;
    if (!contains(out.json, "\"image\":")) return false;
    if (!contains(out.json, "yolo_load_failure")) return false;
    if (!contains(out.json, "img.jpg")) return false;
    return true;
}

bool test_face_infer_concurrent_model_key_switch() {
    // 并发切换模型 key：修复后缓存为 shared_ptr 且调用期局部持有，无悬空访问。
    // 无有效模型（opencv backend 无 .onnx）时 load 失败不缓存，UAF 真实复现
    // 需可加载模型（本地 ncnn 模型需 ncnn 支持）；本测试验证并发调用不崩溃、结果确定。
    const auto dir = makeTempDir("concurrent_key_switch");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    std::atomic<int> okCount{0};
    const int kThreads = 8;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; i++) {
        threads.emplace_back([&, i] {
            FaceInferRequest req;
            req.imagePath = imgPath.string();
            req.fakeDetect = false;
            req.fakeEmbedding = true;
            req.yoloBackend = "opencv";
            req.yoloModelPath = "/nonexistent/yolo_" + std::to_string(i) + ".onnx";
            const auto out = runFaceInferOnce(req);
            if (out.ok) okCount.fetch_add(1);
        });
    }
    for (auto& t : threads) t.join();
    // 全部应 yolo_load 失败（模型不存在），okCount==0；并发调用不崩溃
    return okCount.load() == 0;
}

bool test_face_infer_concurrent_key_switch_keeps_in_use_detector() {
    // 判别性验证：线程 A 命中缓存 K1 并在 detect 中阻塞，线程 B 切换到 K2。
    // 若调用期未持有共享所有权（修复前 localDet 于 if 块末析构），B 替换缓存会销毁
    // 正在使用的 K1 实例（UAF 窗口）；本测试断言 K1 在使用中不被销毁。
    const auto dir = makeTempDir("concurrent_key_switch_disc");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    ModelRegistry::instance().registerDetector(
        "fake_det",
        []() -> std::unique_ptr<FaceDetector> { return std::make_unique<FakeDetector>(); },
        ModelEntry{"fake_det", "FakeDetector", "detect", "B06-008 判别测试", "balanced", 0});

    g_detEntered.store(0);
    g_detRelease.store(false);
    g_k1Destroyed.store(false);

    std::thread tA([&] {
        FaceInferRequest req;
        req.imagePath = imgPath.string();
        req.fakeDetect = false;
        req.fakeEmbedding = true;
        req.detectorId = "fake_det";  // §I3：装配按 detectorId 路由（不再用 yoloBackend 当注册表 key）
        req.yoloModelPath = "/fake/K1.onnx";
        runFaceInferOnce(req);
    });

    // 等 A 进入 detect（K1 已建立缓存）
    {
        std::unique_lock<std::mutex> lk(g_detMu);
        g_detCv.wait(lk, [] { return g_detEntered.load() >= 1; });
    }

    // B 切换缓存 key 到 K2（若 A 未持有所有权，K1 在此被销毁）
    std::thread tB([&] {
        FaceInferRequest req;
        req.imagePath = imgPath.string();
        req.fakeDetect = false;
        req.fakeEmbedding = true;
        req.detectorId = "fake_det";  // §I3：装配按 detectorId 路由
        req.yoloModelPath = "/fake/K2.onnx";
        runFaceInferOnce(req);
    });

    // 等 B 完成缓存替换（进入 detect）
    {
        std::unique_lock<std::mutex> lk(g_detMu);
        g_detCv.wait(lk, [] { return g_detEntered.load() >= 2; });
    }

    // 关键判别：使用中的 K1 实例不得被销毁（修复后 localDet 函数级持有）
    const bool k1Alive = !g_k1Destroyed.load();

    g_detRelease.store(true);
    g_detCv.notify_all();
    tA.join();
    tB.join();

    return k1Alive;
}

bool test_face_infer_done_no_face() {
    const auto dir = makeTempDir("done_no_face");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    FaceInferRequest req;
    req.imagePath = imgPath.string();
    req.fakeDetect = true;
    req.fakeEmbedding = true;
    req.faceSelectPolicy = "fake_none";

    const auto out = runFaceInferOnce(req);
    if (!out.ok) return false;
    if (out.stage != "done") return false;
    if (out.message != "no_face_detected") return false;
    if (!contains(out.json, "\"face\":{")) return false;
    if (!contains(out.json, "\"hasFace\":false")) return false;
    return true;
}

bool test_face_infer_done_hit() {
    const auto dir = makeTempDir("done_hit");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    const auto galleryDir = dir / "gallery";
    std::error_code ec;
    std::filesystem::create_directories(galleryDir, ec);

    const auto fakeEmb = FaceInferStages::makeFakeEmbedding512(cv::Rect2f(16.0f, 16.0f, 32.0f, 32.0f), cv::Size(64, 64));
    FaceTemplate t;
    t.embedding = fakeEmb;
    auto bytes = serializeFaceTemplate(t);
    if (!writeAllBytes(galleryDir / "p1.bin", bytes)) return false;

    FaceInferRequest req;
    req.imagePath = imgPath.string();
    req.fakeDetect = true;
    req.fakeEmbedding = true;
    req.galleryDir = galleryDir.string();
    req.topK = 1;
    req.acceptThreshold = 0.0f;

    const auto out = runFaceInferOnce(req);
    if (!out.ok) return false;
    if (out.stage != "done") return false;
    if (!contains(out.json, "\"TopK\":{")) return false;
    if (!contains(out.json, "\"id\":\"p1\"")) return false;
    return true;
}

// §I3：装配按 embedderId 路由到对应适配器，并把请求节点名经 configure() 透传到适配器。
// 判别点（捕获发生在**替身适配器的 load() 内**）：
//   · 若装配未按 embedderId 路由 / 未调用 configure() ⇒ 读到 ArcFaceAdapter 默认节点名
//     `data`/`output`，断言红；
//   · 若 configure() 未透传节点名 ⇒ 同上，红。
bool test_face_infer_routes_embedder_id_and_configures_arcface() {
    const auto dir = makeTempDir("embedder_route");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    // 先用内建条目注册完，再用替身覆盖 `arcface`（ensureBuiltinRegistered 是一次性的）。
    ModelRegistry::ensureBuiltinRegistered();
    ModelRegistry::instance().registerEmbedder(
        "arcface",
        []() -> std::unique_ptr<Embedder> { return std::make_unique<NodeNameCapturingArcFaceAdapter>(); },
        ModelEntry{"arcface", "Capturing ArcFace", "recognize", "装配路由替身（测试用）", "high_speed", 2});

    {
        std::lock_guard<std::mutex> lk(g_int8Mu);
        g_int8LoadCalls = 0;
        g_int8InputName.clear();
        g_int8OutputName.clear();
    }

    FaceInferRequest req;
    req.imagePath = imgPath.string();
    req.fakeDetect = true;       // 只关心嵌入段
    req.fakeEmbedding = false;   // 必须走真实嵌入装配
    req.embedderId = "arcface";
    req.arcBackend = "ncnn";                 // 节点名经 configure 透传（ncnn 侧字段）
    req.arcNcnnInput = "in0";
    req.arcNcnnOutput = "out0";
    req.arcModelPath = (dir / "embedder_route.param").string();  // 路径不存在 ⇒ 适配器 load 失败

    runFaceInferOnce(req);
    {
        std::lock_guard<std::mutex> lk(g_int8Mu);
        if (g_int8LoadCalls != 1) return false;      // 装配必须调用一次 load
        if (g_int8InputName != "in0") return false;  // configure 透传了请求节点名
        if (g_int8OutputName != "out0") return false;
    }

    // 还原内建 arcface 工厂（本覆盖在进程内持续；不还原会污染后续用例）。
    ModelRegistry::instance().reloadEmbedder(
        "arcface",
        []() -> std::unique_ptr<Embedder> { return std::make_unique<ArcFaceAdapter>(); });
    return true;
}

// FACE-TEST-005①：embed 失败时 **err 必须带出差异化文本**，不得丢弃成固定文案。
//
// 为什么之前没有守护：上一批把 `FaceInferStages.cpp` FP32 回落链尾的 `failStatus("arc_embed",
// "arc_embed_failed")` 改成「有 err 就带出」，但变异性扫掠实测「改回硬编码 ⇒ core_* 全绿」
// ⇒ 该口径**无判别用例**。本用例补上**可达**那条（注入路径，与回落链尾同口径）的守护。
//
// 怎么造出确定性的「embed 失败且 err 非空」：用一个**极小合成 ONNX**（`tests/fixtures/onnx/`，
// 生成脚本与期望 sha256 见该目录）——`readNet` 接受它（故 `ArcFaceEmbedder::initialize` 成功），
// 但它是 `Identity`，前向把 `1×3×112×112` 原样透传 ⇒ 输出 37632 个元素 ≠ 512 ⇒ 命中
// `takeFirst512AsF32` 的 `total != 512`，而 `ArcFaceEmbedder` 的**每一条**失败路径都写 `*err`。
//
// 判别点：把 `failStatus("arc_embed", …)` 的 err 支改回硬编码 `"arc_embed_failed"` ⇒ 本用例红。
//
// ⚠️ 如实登记两处边界：
//   ① 本用例走的是 **ctx.embedder 非空**的**注入路径**（`FaceInferStages.cpp` 的 embed 分支），
//      **不是** FP32 回落链尾那一处。后者在现有架构下**不可达**：Pipeline 级与 Stage 级用**同一份
//      cfg / 同一路径**构建嵌入器 ⇒ 「Pipeline 级 init 失败（ctx.embedder 为 null）」必然蕴含
//      「Stage 级 init 也失败」⇒ 控制流停在 `arc_init` 出口，永远到不了链尾。该结论已用「把链尾
//      改成哨兵文本后整套 C++ 用例无一变化」实测印证。
//   ② 固定件缺失 ⇒ **报错返回 false**（固定件在库内，缺失即真错），不静默跳过。
bool test_face_infer_embed_failure_carries_differential_err() {
    const std::filesystem::path model = "tests/fixtures/onnx/tiny_identity_1x512.onnx";
    if (!std::filesystem::exists(model)) {
        std::cerr << "[face_infer_embed_err] 前置缺失（固定件应在库内）：" << model.string()
                  << "（ctest 的 WORKING_DIRECTORY 已设为源码根；手工运行时请在仓库根执行）" << std::endl;
        return false;
    }

    const auto dir = makeTempDir("fp32_embed_err");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    FaceInferRequest req;
    req.imagePath = imgPath.string();
    req.fakeDetect = true;      // 只关心嵌入段，避免依赖检测模型
    req.fakeEmbedding = false;  // 关键：必须走真实嵌入路径
    req.int8Enabled = false;
    req.arcBackend = "opencv";
    req.arcModelPath = model.string();  // 合成件：initialize 成功、embed 必失败（见上）

    const auto out = runFaceInferOnce(req);

    // 判别点 1：必须真的走到 embed 段并以失败收场（否则本用例的判据不在射程内）。
    if (out.ok) {
        std::cerr << "[face_infer_embed_err] 期望 embed 失败，实际 ok（合成件或链路口径已变）" << std::endl;
        return false;
    }
    if (out.stage != "arc_embed") {
        std::cerr << "[face_infer_embed_err] 期望 stage=arc_embed，实际=" << out.stage
                  << "（若停在 arc_init，说明嵌入器 initialize 未成功）" << std::endl;
        return false;
    }

    // 判别点 2（本用例的核心）：err 必须是上游带出的差异化文本，**不得**是固定回落文案。
    if (out.message.empty()) {
        std::cerr << "[face_infer_embed_err] err 为空——失败静默，违反「失败不静默」" << std::endl;
        return false;
    }
    if (out.message == "arc_embed_failed") {
        std::cerr << "[face_infer_embed_err] err 是固定文案 arc_embed_failed ⇒ 上游差异化文本被丢弃"
                     "（这正是本用例要拦的回退）" << std::endl;
        return false;
    }
    return true;
}

// §I3（由原 FACE-PIPE-INT8-DIM 用例改造）：统一 Embedder 接口**不得**把「输出维度不符」当成功。
// 背景：装配集中后 ctx.embedder 类型改为 rk_core::Embedder*，旧 ArcFaceEmbedder 的
//「has_value() 蕴含 512 维」静态保证对派生实现不再成立 ⇒ computeEmbedding 必须**显式校验维度**，
// 不符即失败（不静默）、返回不含身份。
// 判别点：注册表替身「加载成功但输出 256 维」——
//   去掉 computeEmbedding 的维度校验 ⇒ 整体 outcome ok=true（静默成功）⇒ 本用例红；
//   保留校验 ⇒ 失败（stage=arc_embed）⇒ 整体 ok=false ⇒ 绿。
// 前置锚 `g_wrongDimEmbedCalls >= 1`：证明真的走到了 embed 分支（防「路径没走到也绿」）。
bool test_face_infer_embedder_dim_mismatch_is_not_silent() {
    const auto dir = makeTempDir("embedder_dim_mismatch");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    ModelRegistry::ensureBuiltinRegistered();
    ModelRegistry::instance().registerEmbedder(
        "arcface",
        []() -> std::unique_ptr<Embedder> { return std::make_unique<WrongDimInt8Adapter>(); },
        ModelEntry{"arcface", "Wrong-dim stub", "recognize", "维度不符替身（测试用）", "high_speed", 2});
    g_wrongDimEmbedCalls.store(0);

    FaceInferRequest req;
    req.imagePath = imgPath.string();
    req.fakeDetect = true;      // 只关心嵌入段
    req.fakeEmbedding = false;  // 必须走真实嵌入装配
    req.embedderId = "arcface";
    req.arcBackend = "opencv";
    req.arcModelPath = (dir / "dim_stub.onnx").string();

    const auto out = runFaceInferOnce(req);

    ModelRegistry::instance().reloadEmbedder(
        "arcface",
        []() -> std::unique_ptr<Embedder> { return std::make_unique<ArcFaceAdapter>(); });

    if (g_wrongDimEmbedCalls.load() < 1) {
        std::cerr << "[embedder_dim] 替身未被调用 ⇒ 本用例没走到 embed 分支，判据不在射程内" << std::endl;
        return false;
    }
    if (out.ok) {
        std::cerr << "[embedder_dim] 嵌入输出 256 维（≠512）却被判成功 ⇒ 失败被静默吞掉" << std::endl;
        return false;
    }
    if (out.stage != "arc_embed") {
        std::cerr << "[embedder_dim] 期望 stage=arc_embed，实际=" << out.stage << std::endl;
        return false;
    }
    return true;
}


// ═══════════════════════════════════════════════════════════════════════════════
// §3.3 判别测试（A16/A4/D4：单次管线统一装配）
// ═══════════════════════════════════════════════════════════════════════════════

// §3.3 核心探针：覆盖 YoloFaceAdapter::load，证明「装配真的调用了被路由到的适配器的 load()」。
// 判别点：把 runFaceInferOnce 里对组装实例的 load 调用删掉/挪位 ⇒ calls 恒 0 ⇒ 红。
class LoadProbeYoloAdapter final : public YoloFaceAdapter {
public:
    explicit LoadProbeYoloAdapter(int& calls) : calls_(calls) {}
    bool load(const std::string&, std::string& err) override {
        ++calls_;
        err = "test_load_was_called";
        return false;
    }
private:
    int& calls_;
};

// 计数检测器：load 成功、detect 返回空（配 fakeEmbedding 时整体 ok=true），用于验证缓存复用/重装。
struct CountingDet final : public FaceDetector {
    bool load(const std::string&, std::string&) override { return true; }
    FaceDetections detect(const cv::Mat&, std::string&) override { return {}; }
    const char* name() const override { return "CountingDet"; }
};

bool test_face_infer_routes_detector_id_and_calls_load() {
    const auto dir = makeTempDir("det_load_probe");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    ModelRegistry::ensureBuiltinRegistered();
    int calls = 0;
    ModelRegistry::instance().reloadDetector(
        "yolo_face",
        [&calls]() -> std::unique_ptr<FaceDetector> { return std::make_unique<LoadProbeYoloAdapter>(calls); });

    FaceInferRequest req;
    req.imagePath = imgPath.string();
    req.fakeDetect = false;
    req.fakeEmbedding = true;
    req.detectorId = "yolo_face";  // 默认 ID；旧后端名仍保留（A4：区分后端名与模型 ID）
    req.yoloBackend = "opencv";
    req.yoloModelPath = (dir / "probe_unique.onnx").string();  // 唯一路径 ⇒ 缓存未命中

    const auto out = runFaceInferOnce(req);
    // 同 spec 再跑一次：失败 load 不得进缓存 ⇒ 再次调用 load（calls 递增）。
    const auto out2 = runFaceInferOnce(req);

    // 还原内建工厂（探针覆盖了内建条目；不还原会污染后续用例）。
    ModelRegistry::instance().reloadDetector(
        "yolo_face",
        []() -> std::unique_ptr<FaceDetector> { return std::make_unique<YoloFaceAdapter>(); });

    if (calls != 2) { std::cerr << "[det_probe] load calls=" << calls << "（期望 2：首装 + 失败不缓存后再装）" << std::endl; return false; }
    if (out.ok || out2.ok) return false;
    if (out.stage != "yolo_load") { std::cerr << "[det_probe] stage=" << out.stage << std::endl; return false; }
    if (!contains(out.json, "test_load_was_called")) return false;
    return true;
}

bool test_face_infer_unknown_ids_fail_without_load() {
    const auto dir = makeTempDir("unknown_ids");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    ModelRegistry::ensureBuiltinRegistered();
    int calls = 0;
    ModelRegistry::instance().reloadDetector(
        "yolo_face",
        [&calls]() -> std::unique_ptr<FaceDetector> { return std::make_unique<LoadProbeYoloAdapter>(calls); });

    // unknown detector id ⇒ 失败且不触达已注册的 yolo_face 工厂
    FaceInferRequest req;
    req.imagePath = imgPath.string();
    req.fakeDetect = false;
    req.fakeEmbedding = true;
    req.detectorId = "no_such_detector_id";
    req.yoloModelPath = (dir / "m.onnx").string();
    const auto out = runFaceInferOnce(req);

    ModelRegistry::instance().reloadDetector(
        "yolo_face",
        []() -> std::unique_ptr<FaceDetector> { return std::make_unique<YoloFaceAdapter>(); });

    if (calls != 0) { std::cerr << "[unknown_id] 未知检测 ID 竟触达 yolo_face 工厂" << std::endl; return false; }
    if (out.ok) return false;
    if (out.stage != "yolo_load") return false;

    // unknown embedder id ⇒ 失败（stage=arc_init）
    FaceInferRequest req2;
    req2.imagePath = imgPath.string();
    req2.fakeDetect = true;
    req2.fakeEmbedding = false;
    req2.embedderId = "no_such_embedder_id";
    const auto out2 = runFaceInferOnce(req2);
    if (out2.ok) return false;
    if (out2.stage != "arc_init") return false;
    return true;
}

bool test_face_infer_arcface_failure_does_not_try_mobilefacenet() {
    const auto dir = makeTempDir("no_mobile_fallback");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    ModelRegistry::ensureBuiltinRegistered();
    int mobileCreates = 0;
    // 计数器只在工厂被调用时递增；返回任意 Embedder 即可（本用例只验证「是否被调用」）。
    ModelRegistry::instance().reloadEmbedder(
        "mobilefacenet",
        [&mobileCreates]() -> std::unique_ptr<Embedder> {
            ++mobileCreates;
            return std::make_unique<ArcFaceAdapter>();
        });

    // embedderId=arcface（模型不存在 ⇒ ArcFace 装配失败）⇒ 绝不能尝试 mobilefacenet 工厂
    FaceInferRequest req;
    req.imagePath = imgPath.string();
    req.fakeDetect = true;
    req.fakeEmbedding = false;
    req.embedderId = "arcface";
    req.arcBackend = "opencv";
    req.arcModelPath = (dir / "no_arcface.onnx").string();
    const auto out = runFaceInferOnce(req);
    if (mobileCreates != 0) {
        std::cerr << "[no_mobile_fallback] ArcFace 失败却调用了 mobilefacenet 工厂（禁止盲试式回落）" << std::endl;
        return false;
    }
    if (out.ok) return false;

    // 显式 embedderId=mobilefacenet ⇒ 工厂才被调用
    FaceInferRequest req2;
    req2.imagePath = imgPath.string();
    req2.fakeDetect = true;
    req2.fakeEmbedding = false;
    req2.embedderId = "mobilefacenet";
    req2.arcModelPath = (dir / "no_mobile.onnx").string();
    runFaceInferOnce(req2);

    ModelRegistry::instance().reloadEmbedder(
        "mobilefacenet",
        []() -> std::unique_ptr<Embedder> { return std::make_unique<ArcFaceAdapter>(); });

    if (mobileCreates < 1) { std::cerr << "[no_mobile_fallback] 显式 mobilefacenet 未调用工厂" << std::endl; return false; }
    return true;
}

bool test_face_infer_detector_cache_reinstalls_on_spec_change() {
    const auto dir = makeTempDir("det_cache_reinstall");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    ModelRegistry::ensureBuiltinRegistered();
    int creates = 0;
    ModelRegistry::instance().reloadDetector(
        "yolo_face",
        [&creates]() -> std::unique_ptr<FaceDetector> { ++creates; return std::make_unique<CountingDet>(); });

    const auto run = [&](const std::string& node) {
        FaceInferRequest req;
        req.imagePath = imgPath.string();
        req.fakeDetect = false;
        req.fakeEmbedding = true;
        req.detectorId = "yolo_face";
        req.yoloBackend = "opencv";
        req.yoloModelPath = (dir / "cache_probe.onnx").string();
        req.yoloNcnnInput = node;  // 参与缓存 key：改变 node 必须重装
        return runFaceInferOnce(req);
    };

    const auto o1 = run("inA");  // 首次装配
    const auto o2 = run("inA");  // 同 spec ⇒ 缓存复用（不再装配）
    const auto o3 = run("inB");  // spec 变（节点名）⇒ 必须重装

    ModelRegistry::instance().reloadDetector(
        "yolo_face",
        []() -> std::unique_ptr<FaceDetector> { return std::make_unique<YoloFaceAdapter>(); });

    if (creates != 2) { std::cerr << "[det_cache] creates=" << creates << "（期望 2：首装 + spec 变化重装）" << std::endl; return false; }
    if (!o1.ok || !o2.ok || !o3.ok) return false;
    return true;
}

// §3.3 缓存 key 完整性（detKey）：装配缓存 key 曾漏掉「OpenCV-DNN spec / 预处理 / ncnn 运行时」
// 一类字段，仅它们不同的两次请求会静默复用首个装配实例（错配）。本用例以「证明会重装」判别。
// 判别点：把 detKey 里新增的任一字段（此处取 yoloLetterbox）去掉 ⇒ 仅该字段变化的第二次请求
//        复用缓存、装配工厂不再被调用 ⇒ creates==1 ⇒ 红；字段在 key 内 ⇒ creates==2 ⇒ 绿。
bool test_face_infer_detector_cache_reinstalls_on_option_change() {
    const auto dir = makeTempDir("det_cache_option");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    ModelRegistry::ensureBuiltinRegistered();
    int creates = 0;
    ModelRegistry::instance().reloadDetector(
        "yolo_face",
        [&creates]() -> std::unique_ptr<FaceDetector> { ++creates; return std::make_unique<CountingDet>(); });

    const auto run = [&](bool letterbox) {
        FaceInferRequest req;
        req.imagePath = imgPath.string();
        req.fakeDetect = false;
        req.fakeEmbedding = true;
        req.detectorId = "yolo_face";
        req.yoloBackend = "opencv";
        req.yoloModelPath = (dir / "cache_option.onnx").string();
        req.yoloLetterbox = letterbox;  // 仅预处理选项不同（此前不在缓存 key 内）
        return runFaceInferOnce(req);
    };

    const auto o1 = run(true);   // 首次装配
    const auto o2 = run(true);   // 同 spec ⇒ 缓存复用（不再装配）
    const auto o3 = run(false);  // 仅 letterbox 变 ⇒ 必须重装

    ModelRegistry::instance().reloadDetector(
        "yolo_face",
        []() -> std::unique_ptr<FaceDetector> { return std::make_unique<YoloFaceAdapter>(); });

    if (creates != 2) { std::cerr << "[det_cache_option] creates=" << creates
                                  << "（期望 2：首装 + 选项变化重装）" << std::endl; return false; }
    if (!o1.ok || !o2.ok || !o3.ok) return false;
    return true;
}

// 计数嵌入器：load 成功、embed 返回合法 512 维，用于验证嵌入缓存复用/重装。
struct CountingEmb final : public Embedder {
    bool load(const std::string&, std::string&) override { return true; }
    std::optional<std::vector<float>> embed(const cv::Mat&, std::string&) override {
        return std::vector<float>(512, 0.1f);
    }
    int embeddingDim() const override { return 512; }
    const char* name() const override { return "CountingEmb"; }
};

// §3.3 缓存 key 完整性（embKey）：同 detKey，embKey 曾漏掉「OpenCV-DNN spec / 预处理版本 /
// ncnn 运行时」字段，仅它们不同的两次请求会静默复用首个嵌入实例（错配）。
// 判别点：把 embKey 里新增的任一字段（此处取 arcPreprocessVersion）去掉 ⇒ 仅该字段变化的
//        第二次请求复用缓存 ⇒ creates==1 ⇒ 红；字段在 key 内 ⇒ creates==2 ⇒ 绿。
bool test_face_infer_embedder_cache_reinstalls_on_option_change() {
    const auto dir = makeTempDir("emb_cache_option");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    ModelRegistry::ensureBuiltinRegistered();
    int creates = 0;
    ModelRegistry::instance().reloadEmbedder(
        "arcface",
        [&creates]() -> std::unique_ptr<Embedder> { ++creates; return std::make_unique<CountingEmb>(); });

    const auto run = [&](std::uint32_t preprocessVersion) {
        FaceInferRequest req;
        req.imagePath = imgPath.string();
        req.fakeDetect = true;
        req.fakeEmbedding = false;
        req.embedderId = "arcface";
        req.arcBackend = "opencv";
        req.arcModelPath = (dir / "cache_emb.onnx").string();
        req.arcPreprocessVersion = preprocessVersion;  // 仅预处理版本不同（此前不在缓存 key 内）
        return runFaceInferOnce(req);
    };

    run(1);  // 首次装配
    run(1);  // 同 spec ⇒ 缓存复用（不再装配）
    run(2);  // 仅 preprocessVersion 变 ⇒ 必须重装

    ModelRegistry::instance().reloadEmbedder(
        "arcface",
        []() -> std::unique_ptr<Embedder> { return std::make_unique<ArcFaceAdapter>(); });

    if (creates != 2) { std::cerr << "[emb_cache_option] creates=" << creates
                                  << "（期望 2：首装 + 选项变化重装）" << std::endl; return false; }
    return true;
}

bool test_face_infer_wrong_backend_for_filetype_fails() {
    const auto dir = makeTempDir("wrong_backend");
    const auto imgPath = writeTestImage(dir, "img.jpg");
    ModelRegistry::ensureBuiltinRegistered();

    // backend=ncnn（本机未编译 ncnn）⇒ 明确失败，不静默换 opencv
    FaceInferRequest req;
    req.imagePath = imgPath.string();
    req.fakeDetect = false;
    req.fakeEmbedding = true;
    req.detectorId = "yolo_face";
    req.yoloBackend = "ncnn";
    req.yoloModelPath = (dir / "wb.onnx").string();
    const auto out = runFaceInferOnce(req);
    if (out.ok) return false;
    if (out.stage != "yolo_load") { std::cerr << "[wrong_backend] ncnn stage=" << out.stage << std::endl; return false; }

    // backend=opencv，但给了 .param（ncnn 文件）⇒ 显式后端不匹配 ⇒ 失败（不按后缀静默换后端）
    FaceInferRequest req2;
    req2.imagePath = imgPath.string();
    req2.fakeDetect = false;
    req2.fakeEmbedding = true;
    req2.detectorId = "yolo_face";
    req2.yoloBackend = "opencv";
    req2.yoloModelPath = (dir / "wb.param").string();
    const auto out2 = runFaceInferOnce(req2);
    if (out2.ok) return false;
    if (out2.stage != "yolo_load") { std::cerr << "[wrong_backend] opencv/.param stage=" << out2.stage << std::endl; return false; }
    return true;
}

// 写合成 profile 目录：manifest + 4 个 dummy 模型文件（不带 sha256 ⇒ verifyFiles 只校验存在性）。
static bool writeProfileDir(const std::filesystem::path& dir, const std::string& abi,
                            const std::string& detPrec, const std::string& embPrec) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto wr = [&](const char* n, const char* c) {
        std::ofstream f(dir / n, std::ios::binary);
        f << c;
    };
    wr("scrfd.param", "dummy");
    wr("scrfd.bin", "dummy");
    wr("arcface.param", "dummy");
    wr("arcface.bin", "dummy");
    std::ofstream m(dir / "manifest.json", std::ios::binary);
    m << "{\n"
      << "  \"schemaVersion\": 1,\n"
      << "  \"profileId\": \"test_profile\",\n"
      << "  \"displayName\": \"Test Profile\",\n"
      << "  \"supportedAbis\": [\"" << abi << "\"],\n"
      << "  \"detector\": {\"param\": {\"path\": \"scrfd.param\"}, \"bin\": {\"path\": \"scrfd.bin\"},"
         " \"input\": \"input.1\", \"outputs\": [\"s8\",\"s16\",\"s32\",\"b8\",\"b16\",\"b32\",\"k8\",\"k16\",\"k32\"],"
         " \"inputWidth\": 640, \"inputHeight\": 640, \"decodeMode\": \"distance\","
         " \"anchorsPerPos\": [2,2,2], \"outputLayout\": \"nchw\", \"precision\": \"" << detPrec << "\"},\n"
      << "  \"embedder\": {\"param\": {\"path\": \"arcface.param\"}, \"bin\": {\"path\": \"arcface.bin\"},"
         " \"input\": \"in0\", \"output\": \"out0\", \"inputWidth\": 112, \"inputHeight\": 112,"
         " \"embeddingDim\": 512, \"precision\": \"" << embPrec << "\"},\n"
      << "  \"detectionConfidence\": 0.5, \"nmsIou\": 0.4, \"identitySimilarity\": 0.45, \"modelVersion\": \"2\"\n"
      << "}\n";
    return m.good();
}

// §3.3：applyFaceProfileToRequest 一次性派生请求字段 + profile 九节点/distance/2anchors/layout
// 实际到达 configFromProfile（供解码消费）。
bool test_face_infer_profile_apply_derives_and_reaches_config() {
    const auto dir = makeTempDir("profile_apply") / "prof";
    if (!writeProfileDir(dir, "windows-x64", "int8", "int8")) return false;

    FaceInferRequest req;
    FaceRuntimeProfile prof;
    std::string err;
    if (!applyFaceProfileToRequest(dir.string(), "windows-x64", req, prof, err)) {
        std::cerr << "[profile_apply] apply failed: " << err << std::endl;
        return false;
    }
    if (req.detectorId != "scrfd_0.5gf") return false;
    if (req.embedderId != "arcface") return false;
    if (req.yoloBackend != "ncnn" || req.arcBackend != "ncnn") return false;
    if (!req.int8Enabled) return false;  // 双 precision=int8 ⇒ true
    if (req.yoloInputW != 640 || req.yoloInputH != 640) return false;
    if (req.arcModelVersion != 2) return false;
    if (req.yoloNcnnParam.find("scrfd.param") == std::string::npos) return false;

    std::string cerr;
    ScrfdNcnnConfig cfg = ScrfdNcnnAdapter::configFromProfile(prof, req.yoloNcnnParam, req.yoloNcnnBin, cerr);
    if (!cerr.empty()) { std::cerr << "[profile_apply] config err=" << cerr << std::endl; return false; }
    if (cfg.decodeMode != pipeline::DecodeMode::kDistance) return false;
    if (cfg.anchorsPerPos[0] != 2 || cfg.anchorsPerPos[1] != 2 || cfg.anchorsPerPos[2] != 2) return false;
    if (cfg.outputLayout != ScrfdNcnnConfig::OutputLayout::kNChw) return false;
    if (cfg.inputName != "input.1") return false;
    if (cfg.scoreNames[0] != "s8" || cfg.bboxNames[0] != "b8" || cfg.kpsNames[2] != "k32") return false;
    if (cfg.inputW != 640 || cfg.inputH != 640) return false;
    if (!(cfg.confThreshold == 0.5f)) return false;
    return true;
}

// §3.1：applyFaceProfileToRequest **失败不改 req**；runFaceInferOnce 在 profile 不可用时 stage=face_profile。
bool test_face_infer_profile_failure_leaves_request_unchanged() {
    const auto dir = makeTempDir("profile_fail") / "prof";
    // manifest 只列 armeabi-v7a：本机编译 ABI（windows-x64）与显式传入的 ABI 都不匹配 ⇒ 必失败。
    if (!writeProfileDir(dir, "armeabi-v7a", "fp32", "fp32")) return false;

    FaceInferRequest req;
    req.detectorId = "keep_me";
    req.yoloBackend = "keep_backend";
    FaceRuntimeProfile prof;
    std::string err;
    // ABI 不匹配（manifest 仅列 armeabi-v7a，这里传 windows-x64）⇒ 失败且不改 req
    const bool ok = applyFaceProfileToRequest(dir.string(), "windows-x64", req, prof, err);
    if (ok) return false;
    if (req.detectorId != "keep_me") return false;
    if (req.yoloBackend != "keep_backend") return false;

    const auto imgDir = makeTempDir("profile_fail_img");
    const auto imgPath = writeTestImage(imgDir, "img.jpg");
    FaceInferRequest req2;
    req2.imagePath = imgPath.string();
    req2.faceProfileDir = dir.string();
    req2.fakeDetect = true;
    req2.fakeEmbedding = true;
    const auto out = runFaceInferOnce(req2);
    if (out.ok) return false;
    if (out.stage != "face_profile") { std::cerr << "[profile_fail] stage=" << out.stage << std::endl; return false; }
    return true;
}
