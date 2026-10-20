#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <net.h>

#include "FileHash.h"
#include "NcnnParamGuard.h"
#include "NcnnPrecisionGuard.h"
#include "adapters/ScrfdNcnnAdapter.h"
#include "rk_win/JsonLite.h"

namespace {
namespace fs = std::filesystem;

struct Options {
    fs::path root = ".";
    fs::path images = "models/calib_images";
    std::string mode = "all";
    int maxImages = 50;
    bool selfTest = false;
    rk_core::ScrfdNcnnConfig detector;
};

Options parseArgs(int argc, char** argv) {
    Options options;
    options.detector.decodeMode = pipeline::DecodeMode::kDistance;
    for (int& anchors : options.detector.anchorsPerPos) anchors = 2;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--self-test") { options.selfTest = true; continue; }
        if (i + 1 == argc) throw std::runtime_error("选项缺少值");
        const std::string value = argv[++i];
        if (key == "--root") options.root = value;
        else if (key == "--images") options.images = value;
        else if (key == "--mode") options.mode = value;
        else if (key == "--max-images") options.maxImages = std::stoi(value);
        else if (key == "--scrfd-input") options.detector.inputName = value;
        else if (key == "--scrfd-outputs") {
            std::istringstream input(value);
            std::string node;
            std::vector<std::string> names;
            while (std::getline(input, node, ',')) names.push_back(node);
            if (names.size() != 9) throw std::runtime_error("SCRFD 必须提供九个输出节点");
            for (int n = 0; n < 3; ++n) {
                options.detector.scoreNames[n] = names[n];
                options.detector.bboxNames[n] = names[n + 3];
                options.detector.kpsNames[n] = names[n + 6];
            }
        } else if (key == "--scrfd-layout") {
            using Layout = rk_core::ScrfdNcnnConfig::OutputLayout;
            if (value == "nchw") options.detector.outputLayout = Layout::kNChw;
            else if (value == "flat") options.detector.outputLayout = Layout::kFlat;
            else if (value == "auto") options.detector.outputLayout = Layout::kAuto;
            else throw std::runtime_error("非法 SCRFD 输出布局");
        } else throw std::runtime_error("未知选项");
    }
    if (options.maxImages <= 0 || (options.mode != "all" && options.mode != "scrfd" && options.mode != "arcface"))
        throw std::runtime_error("非法模型模式或图片数");
    return options;
}

double cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return 0.0;
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    const double denominator = std::sqrt(na) * std::sqrt(nb);
    return denominator > 1e-10 ? dot / denominator : 0.0;
}

// 精度程序与生产 ArcFace 契约共用 RGB/127.5/1÷127.5 口径，独立于 SCRFD 的 1÷128。
ncnn::Mat preprocessArcFaceImage(const cv::Mat& bgr) {
    const float meanVals[3] = {127.5f, 127.5f, 127.5f};
    const float normVals[3] = {1.f / 127.5f, 1.f / 127.5f, 1.f / 127.5f};
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    ncnn::Mat input = ncnn::Mat::from_pixels_resize(rgb.data, ncnn::Mat::PIXEL_RGB,
                                                  rgb.cols, rgb.rows, 112, 112);
    input.substract_mean_normalize(meanVals, normVals);
    return input;
}

bool testArcFacePreprocessingContract() {
    const auto input = preprocessArcFaceImage(cv::Mat(1, 1, CV_8UC3, cv::Scalar(0, 128, 255)));
    return input.w == 112 && input.h == 112 && input.c == 3 &&
           std::abs(static_cast<const float*>(input.channel(0))[0] - 1.0f) < 1e-6f &&
           std::abs(static_cast<const float*>(input.channel(1))[0] - 0.5f / 127.5f) < 1e-6f &&
           std::abs(static_cast<const float*>(input.channel(2))[0] + 1.0f) < 1e-6f;
}

struct DetectionMetrics {
    std::size_t reference = 0, predicted = 0, matched = 0;
    double iouSum = 0;
    double precision() const { return predicted ? static_cast<double>(matched) / predicted : 0; }
    double recall() const { return reference ? static_cast<double>(matched) / reference : 0; }
    double meanIou() const { return matched ? iouSum / matched : 0; }
    bool acceptable() const { return matched > 0 && precision() >= 0.90 && recall() >= 0.90 && meanIou() >= 0.80; }
};

DetectionMetrics compareDetections(const FaceDetections& reference, const FaceDetections& predicted) {
    struct Match { double iou; std::size_t reference, predicted; };
    std::vector<Match> candidates;
    for (std::size_t r = 0; r < reference.size(); ++r) {
        for (std::size_t p = 0; p < predicted.size(); ++p) {
            const auto overlap = reference[r].bbox & predicted[p].bbox;
            const double intersection = std::max(0.0f, overlap.width) * std::max(0.0f, overlap.height);
            const double unionArea = reference[r].bbox.area() + predicted[p].bbox.area() - intersection;
            const double iou = unionArea > 0 ? intersection / unionArea : 0;
            if (std::isfinite(iou) && iou >= 0.50) candidates.push_back({iou, r, p});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Match& a, const Match& b) { return a.iou > b.iou; });
    std::vector<bool> usedReference(reference.size()), usedPredicted(predicted.size());
    DetectionMetrics result{reference.size(), predicted.size()};
    for (const auto& candidate : candidates) {
        if (usedReference[candidate.reference] || usedPredicted[candidate.predicted]) continue;
        usedReference[candidate.reference] = usedPredicted[candidate.predicted] = true;
        ++result.matched;
        result.iouSum += candidate.iou;
    }
    return result;
}

bool testDetectionMetricContract() {
    FaceDetection face;
    face.bbox = cv::Rect2f(10, 10, 20, 20);
    const FaceDetections reference{face};
    if (!compareDetections(reference, reference).acceptable()) return false;
    const auto duplicate = compareDetections(reference, FaceDetections{face, face});
    if (duplicate.matched != 1 || duplicate.acceptable() || duplicate.precision() != 0.5) return false;
    face.bbox.x = 100;
    return !compareDetections(reference, FaceDetections{face}).acceptable() &&
           !compareDetections({}, {}).acceptable() && !compareDetections(reference, {}).acceptable();
}

struct ModelPair { fs::path fp32Param, fp32Bin, int8Param, int8Bin; };

std::string requiredString(const rk_win::JsonValue& report, const std::string& key) {
    const auto* value = report.find(key);
    if (!value || !value->isString() || value->s.empty()) throw std::runtime_error("量化报告字段缺失: " + key);
    return value->s;
}

ModelPair loadPair(const fs::path& reportPath, const std::string& model) {
    std::ifstream file(reportPath);
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    rk_win::JsonValue report;
    std::string error;
    if (!rk_win::parseJson(text, report, error) || !report.isObject()) throw std::runtime_error("量化报告不可解析");
    if (requiredString(report, "model") != model) throw std::runtime_error("量化报告架构不匹配");
    if (model == "scrfd" && requiredString(report, "geometry") != "letterbox_center_pad0")
        throw std::runtime_error("SCRFD 校准几何与生产不一致");
    ModelPair pair;
    const std::pair<const char*, fs::path*> paths[] = {
        {"fp32_param", &pair.fp32Param}, {"fp32_bin", &pair.fp32Bin},
        {"int8_param", &pair.int8Param}, {"int8_bin", &pair.int8Bin}};
    for (const auto& item : paths) {
        *item.second = reportPath.parent_path() / requiredString(report, item.first);
        const auto hash = rk_wcfr::calculateSHA256(*item.second);
        if (hash.empty() || hash != requiredString(report, std::string(item.first) + "_sha256"))
            throw std::runtime_error("量化父模型或产物摘要不匹配");
    }
    if (!rk_core::satisfiesInt8Requirement("ncnn", pair.int8Param.string(), error))
        throw std::runtime_error("INT8 产物缺少实际量化层: " + error);
    if (rk_core::satisfiesInt8Requirement("ncnn", pair.fp32Param.string(), error))
        throw std::runtime_error("FP32 父模型实际含 INT8 层");
    return pair;
}

bool loadNet(ncnn::Net& net, const fs::path& param, const fs::path& bin) {
    std::string error;
    return rk_core::validateNcnnParamFile(param.string().c_str(), error) &&
           net.load_param(param.string().c_str()) == 0 && net.load_model(bin.string().c_str()) == 0;
}

std::vector<float> embedding(ncnn::Net& net, const ncnn::Mat& input) {
    ncnn::Extractor extractor = net.create_extractor();
    ncnn::Mat output;
    if (extractor.input("in0", input) != 0 || extractor.extract("out0", output) != 0 || output.empty())
        throw std::runtime_error("ArcFace 节点或推理失败");
    std::vector<float> values;
    for (int channel = 0; channel < output.c; ++channel) {
        const float* data = output.channel(channel);
        values.insert(values.end(), data, data + output.w * output.h * output.d);
    }
    return values;
}

bool testArcFacePrecision(const ModelPair& pair, const std::vector<fs::path>& images) {
    ncnn::Net fp32, int8;
    if (!loadNet(fp32, pair.fp32Param, pair.fp32Bin) || !loadNet(int8, pair.int8Param, pair.int8Bin)) return false;
    double minimum = 1.0, sum = 0;
    for (const auto& image : images) {
        const auto bgr = cv::imread(image.string());
        if (bgr.empty()) throw std::runtime_error("测试图片不可读取");
        const auto input = preprocessArcFaceImage(bgr);
        const double similarity = cosineSimilarity(embedding(fp32, input), embedding(int8, input));
        minimum = std::min(minimum, similarity);
        sum += similarity;
    }
    std::cout << "PRECISION_METRIC model=arcface reference=fp32 images=" << images.size()
              << " cosine_min=" << minimum << " cosine_mean=" << sum / images.size() << '\n';
    return minimum >= 0.90;
}

bool testDetectPrecision(const ModelPair& pair, const std::vector<fs::path>& images,
                         rk_core::ScrfdNcnnConfig config) {
    config.paramPath = pair.fp32Param.string(); config.binPath = pair.fp32Bin.string();
    rk_core::ScrfdNcnnAdapter fp32(config);
    config.paramPath = pair.int8Param.string(); config.binPath = pair.int8Bin.string();
    rk_core::ScrfdNcnnAdapter int8(config);
    std::string error;
    if (!fp32.load(pair.fp32Param.string(), error) || !int8.load(pair.int8Param.string(), error))
        throw std::runtime_error("SCRFD 模型加载失败: " + error);
    DetectionMetrics total;
    for (const auto& image : images) {
        const auto bgr = cv::imread(image.string());
        if (bgr.empty()) throw std::runtime_error("测试图片不可读取");
        const auto reference = fp32.detect(bgr, error);
        if (!error.empty()) throw std::runtime_error("FP32 SCRFD 失败: " + error);
        const auto predicted = int8.detect(bgr, error);
        if (!error.empty()) throw std::runtime_error("INT8 SCRFD 失败: " + error);
        const auto current = compareDetections(reference, predicted);
        total.reference += current.reference; total.predicted += current.predicted;
        total.matched += current.matched; total.iouSum += current.iouSum;
    }
    // 这里只衡量相对同父 FP32 的一致性；无标注真值时不得称为 mAP/真实召回率。
    std::cout << "PRECISION_METRIC model=scrfd reference=fp32 images=" << images.size()
              << " fp32_faces=" << total.reference << " int8_faces=" << total.predicted
              << " matched=" << total.matched << " match_iou_threshold=0.5"
              << " relative_precision=" << total.precision() << " relative_recall=" << total.recall()
              << " matched_mean_iou=" << total.meanIou() << '\n';
    return total.acceptable();
}

std::vector<fs::path> imageFiles(const fs::path& directory, int limit) {
    std::vector<fs::path> images;
    if (!fs::is_directory(directory)) return images;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        auto extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
        if (extension == ".jpg" || extension == ".jpeg" || extension == ".png" || extension == ".bmp")
            images.push_back(entry.path());
    }
    std::sort(images.begin(), images.end());
    if (images.size() > static_cast<std::size_t>(limit)) images.resize(limit);
    return images;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseArgs(argc, argv);
        if (!testArcFacePreprocessingContract() || !testDetectionMetricContract()) {
            std::cerr << "TEST_FAIL name=ncnn_precision_contracts\n";
            return 1;
        }
        if (options.selfTest) {
            std::cout << "TEST_PASS name=ncnn_precision_contracts\n";
            return 0;
        }
        const auto images = imageFiles(options.root / options.images, options.maxImages);
        int pass = 0, fail = 0, unverified = 0;
        for (const std::string model : {"scrfd", "arcface"}) {
            if (options.mode != "all" && options.mode != model) continue;
            const auto report = options.root / "models" / (model + "_int8_ncnn") / "quantize_report.json";
            if (images.empty() || !fs::is_regular_file(report)) {
                ++unverified;
                std::cout << "TEST_UNVERIFIED model=" << model << " reason=missing_model_or_evaluation_images\n";
                continue;
            }
            try {
                const auto pair = loadPair(report, model);
                const bool ok = model == "scrfd" ? testDetectPrecision(pair, images, options.detector)
                                                  : testArcFacePrecision(pair, images);
                ok ? ++pass : ++fail;
                std::cout << (ok ? "TEST_PASS" : "TEST_FAIL") << " name=ncnn_precision_" << model << '\n';
            } catch (const std::exception& error) {
                ++fail;
                std::cerr << "TEST_FAIL name=ncnn_precision_" << model << " reason=" << error.what() << '\n';
            }
        }
        std::cout << "TEST_SUMMARY pass=" << pass << " fail=" << fail << " unverified=" << unverified << '\n';
        return fail ? 1 : unverified ? 77 : 0;
    } catch (const std::exception& error) {
        std::cerr << "TEST_FAIL reason=" << error.what() << '\n';
        return 2;
    }
}
