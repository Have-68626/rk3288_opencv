#include "FaceInferencePipeline.h"

#include "FaceInferStages.h"

#include "FaceTemplate.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <filesystem>
#include <fstream>
#include <string>
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

