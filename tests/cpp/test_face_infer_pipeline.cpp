#include "FaceInferencePipeline.h"

using namespace rk_core;

#include "FaceDetector.h"
#include "FaceInferStages.h"
#include "FaceTemplate.h"
#include "ModelRegistry.h"
#include "adapters/ArcFaceAdapter.h"

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
        req.yoloBackend = "fake_det";
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
        req.yoloBackend = "fake_det";
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

// §INT8 链：`int8Enabled` 的消费可达性 + `arcface_int8` 分支的节点名注入（详见文件上方替身类注释）。
bool test_face_infer_int8_chain_consumes_flag_and_injects_arcface_node_names() {
    const auto dir = makeTempDir("int8_chain");
    const auto imgPath = writeTestImage(dir, "img.jpg");

    // ⚠️ 必须先让内建条目注册完，再用替身**覆盖** `arcface_int8`：`ensureBuiltinRegistered()` 是一次性的，
    //    若顺序反过来，首次进入 computeEmbedding 时内建注册会把替身覆盖掉（本机存在
    //    `models/arcface_int8_ncnn/arcface_int8.param` 时尤其如此）⇒ 用例随机失效。
    ModelRegistry::ensureBuiltinRegistered();
    ModelRegistry::instance().registerEmbedder(
        "arcface_int8",
        []() -> std::unique_ptr<Embedder> { return std::make_unique<NodeNameCapturingArcFaceAdapter>(); },
        ModelEntry{"arcface_int8", "Capturing ArcFace INT8", "recognize", "INT8 链替身（测试用）", "high_speed", 2});
    // 注：本覆盖在**进程内持续**（注册表无「恢复内建条目」的公开入口）。后续用例都不走 INT8 路径
    //     （INT8 分支只在 `int8Enabled == true` 且上游注入失败时才可达），故无交叉影响。

    const auto makeReq = [&](bool int8) {
        FaceInferRequest req;
        req.imagePath = imgPath.string();
        req.fakeDetect = true;      // 走假检测：本用例只关心嵌入段，避免依赖检测模型
        req.fakeEmbedding = false;  // 关键：必须走真实嵌入路径，否则 computeEmbedding 在开头直接返回
        req.int8Enabled = int8;
        req.arcBackend = "opencv";
        // 「开」侧路径取 **`.param`**（而非 `.onnx`）：INT8 回落段只在**本次 load 会选到 ncnn 后端**
        // 时才注入节点名（判据 = 扩展名 `.param`/`.bin` → ncnn，其余 → OpenCvDnn；闸门见
        // src/cpp/src/FaceInferStages.cpp 的 `arcModelIsNcnn`，理由是该处的 `setNodeNames` 会
        // **同时**改写 opencv 侧节点名，对 `.onnx` 强制 "in0" 会把 opencv 侧默认的「空串 = 自动推断」
        // 改坏）。故用 `.onnx` 作「开」侧路径时本用例的注入断言**不再成立**；改用 `.param` 后既踩到
        // 闸门内的真实生产路径，也与 `rk3288_cli --int8 1` 的文档前置条件（ncnn 模型）一致。
        // 路径不存在 ⇒ 上游 ArcFaceEmbedder 注入失败（ctx.embedder 被赋 nullptr）⇒ 才可达 INT8 回落段。
        // 两侧取不同路径，避免命中 FaceInferencePipeline 的静态嵌入器缓存。
        req.arcModelPath = (dir / (int8 ? "int8_chain_on.param" : "int8_chain_off.onnx")).string();
        return req;
    };

    {
        std::lock_guard<std::mutex> lk(g_int8Mu);
        g_int8LoadCalls = 0;
        g_int8InputName.clear();
        g_int8OutputName.clear();
    }

    // 对照（int8Enabled=false）：不得触碰 INT8 条目 —— 没有这一半，下面的断言即使实现退化成
    // 「永远尝试 INT8」也照样绿，断言将失去判别力。
    const auto offOut = runFaceInferOnce(makeReq(false));
    {
        std::lock_guard<std::mutex> lk(g_int8Mu);
        if (g_int8LoadCalls != 0) return false;
    }
    // 无真实嵌入模型 ⇒ 应停在 arc_init（不强制具体 stage：构建未启 ncnn / OpenCV 版本差异都不影响本断言）。
    if (offOut.ok) return false;

    // 实验（int8Enabled=true）：INT8 条目必须被加载，且节点名必须是注入后的 in0/out0。
    runFaceInferOnce(makeReq(true));
    {
        std::lock_guard<std::mutex> lk(g_int8Mu);
        if (g_int8LoadCalls == 0) return false;
        if (g_int8InputName != "in0") return false;
        if (g_int8OutputName != "out0") return false;
    }
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

