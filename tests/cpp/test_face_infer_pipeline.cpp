#include "FaceInferencePipeline.h"

using namespace rk_core;

#include "FaceDetector.h"
#include "FaceInferStages.h"
#include "FaceTemplate.h"
#include "ModelRegistry.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
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

