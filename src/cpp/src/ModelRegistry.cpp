#include "ModelRegistry.h"
#include "adapters/ArcFaceAdapter.h"
#include "adapters/YoloFaceAdapter.h"
#include "adapters/MobileFaceNetAdapter.h"
#include "adapters/YuNetAdapter.h"
#include "adapters/SFaceAdapter.h"
#ifdef _WIN32
#include "adapters/DnnSsdAdapter.h"
#include "adapters/CascadeAdapter.h"
#include "adapters/LbphAdapter.h"
#endif

#include <algorithm>
#include <filesystem>
#include <unordered_map>

ModelRegistry& ModelRegistry::instance() {
    static ModelRegistry reg;
    return reg;
}

static bool g_builtinRegistered = false;

void ModelRegistry::ensureBuiltinRegistered() {
    if (g_builtinRegistered) return;
    g_builtinRegistered = true;
    auto& reg = instance();

    reg.registerDetector("yolo_face", []() { return std::make_unique<YoloFaceAdapter>(); },
        {"yolo_face", "YOLO Face Detector", "detect",
         "YOLOv5 架构，320x320 输入，精度高。适合光照良好、角度正面的场景。RK3288 ncnn 约 15ms/帧。",
         "high_accuracy", 1});

    reg.registerDetector("scrfd_0.5gf", []() { return std::make_unique<YoloFaceAdapter>(); },
        {"scrfd_0.5gf", "SCRFD-0.5GF", "detect",
         "轻量级检测器，0.5G FLOPs，WiderFace 96.1% AP。适合资源受限设备。需要额外下载 ncnn 模型。",
         "high_speed", 2});

    reg.registerEmbedder("arcface", []() { return std::make_unique<ArcFaceAdapter>(); },
        {"arcface", "ArcFace 512D", "recognize",
         "ArcFace 512 维特征提取，LFW 99.8%。精度优先方案，适合门禁/人证比对。RK3288 ncnn 约 8ms/帧。",
         "high_accuracy", 1});

    reg.registerEmbedder("mobilefacenet", []() { return std::make_unique<MobileFaceNetAdapter>(); },
        {"mobilefacenet", "MobileFaceNet 128D", "recognize",
         "MobileFaceNet 128 维嵌入，推理快 2-3 倍。适合 RK3288 等资源受限设备。需额外下载模型。",
         "high_speed", 2});

#ifdef _WIN32
    reg.registerDetector("dnn_ssd", []() { return std::make_unique<DnnSsdAdapter>(); },
        {"dnn_ssd", "OpenCV DNN SSD Face Detector", "detect",
         "ResNet SSD 300x300，OpenCV DNN 后端检测器。Windows 管线内置，精度高于 Cascade。",
         "balanced", 3});

    reg.registerDetector("cascade_lbp", []() { return std::make_unique<CascadeAdapter>(); },
        {"cascade_lbp", "LBP Cascade Face Detector", "detect",
         "LBP 级联分类器，极轻量。适合低资源环境，精度低于 DNN 方案。Windows 管线默认检测器。",
         "high_speed", 4});

    reg.registerEmbedder("lbph", []() { return std::make_unique<LbphAdapter>(); },
        {"lbph", "LBPH Face Recognizer", "recognize",
         "LBP 直方图识别器，轻量但精度有限（<90%）。Windows 管线默认识别器。",
         "high_speed", 3});
#endif

    reg.registerDetector("yunet", []() { return std::make_unique<YuNetAdapter>(); },
        {"yunet", "YuNet Face Detector", "detect",
         "OpenCV FaceDetectorYN，320x320 输入。多尺度 FPN+SSH，WiderFace 91.5%。OpenCV 内置，无需额外模型依赖。",
         "balanced", 3});

    reg.registerEmbedder("sface", []() { return std::make_unique<SFaceAdapter>(); },
        {"sface", "SFace 128D", "recognize",
         "SFace 128 维特征提取，CPU 上最快的识别模型。OpenCV contrib face 模块内置。",
         "high_speed", 2});

    // INT8 量化模型注册 — 需要 INT8 模型文件存在才注册
    auto fileExists = [](const std::string& path) -> bool {
        std::error_code ec;
        return std::filesystem::exists(path, ec) && !ec;
    };

    if (fileExists("models/yolo_face_int8_ncnn/yolo_face_int8.param")) {
        reg.registerDetector("yolo_face_int8", []() {
            return std::make_unique<YoloFaceAdapter>();
        },
        {"yolo_face_int8", "YOLO Face INT8", "detect",
         "YOLOv5 INT8 量化模型，ncnn 后端。体积小、推理快，适合 RK3288 等资源受限设备。",
         "high_speed", 2});
    }

    if (fileExists("models/arcface_int8_ncnn/arcface_int8.param")) {
        reg.registerEmbedder("arcface_int8", []() {
            return std::make_unique<ArcFaceAdapter>();
        },
        {"arcface_int8", "ArcFace INT8 128D", "recognize",
         "ArcFace INT8 量化模型，128 维嵌入。推理快、体积小，适合嵌入式部署。",
         "high_speed", 2});
    }

    if (fileExists("models/mobilefacenet_int8_ncnn/mobilefacenet_int8.param")) {
        reg.registerEmbedder("mobilefacenet_int8", []() {
            return std::make_unique<MobileFaceNetAdapter>();
        },
        {"mobilefacenet_int8", "MobileFaceNet INT8 128D", "recognize",
         "MobileFaceNet INT8 量化模型，128 维嵌入。极轻量，适合资源极度受限设备。",
         "high_speed", 3});
    }
}

void ModelRegistry::registerDetector(const std::string& id, DetectorFactory factory, const ModelEntry& entry) {
    detectors_[id] = DetectorSlot{std::move(factory), entry};
}

void ModelRegistry::registerEmbedder(const std::string& id, EmbedderFactory factory, const ModelEntry& entry) {
    embedders_[id] = EmbedderSlot{std::move(factory), entry};
}

std::unique_ptr<FaceDetector> ModelRegistry::createDetector(const std::string& id, std::string* err) {
    auto it = detectors_.find(id);
    if (it == detectors_.end()) {
        if (err) *err = "detector_not_found: " + id;
        return nullptr;
    }
    return it->second.factory();
}

std::unique_ptr<Embedder> ModelRegistry::createEmbedder(const std::string& id, std::string* err) {
    auto it = embedders_.find(id);
    if (it == embedders_.end()) {
        if (err) *err = "embedder_not_found: " + id;
        return nullptr;
    }
    return it->second.factory();
}

const ModelEntry* ModelRegistry::getEntry(const std::string& id) const {
    {
        auto it = detectors_.find(id);
        if (it != detectors_.end()) return &it->second.entry;
    }
    {
        auto it = embedders_.find(id);
        if (it != embedders_.end()) return &it->second.entry;
    }
    return nullptr;
}

std::vector<ModelEntry> ModelRegistry::listAll() const {
    std::vector<ModelEntry> result;
    result.reserve(detectors_.size() + embedders_.size());
    for (const auto& p : detectors_) result.push_back(p.second.entry);
    for (const auto& p : embedders_) result.push_back(p.second.entry);
    return result;
}

std::vector<ModelEntry> ModelRegistry::listByTask(const std::string& taskType) const {
    std::vector<ModelEntry> result;
    for (const auto& p : detectors_) {
        if (p.second.entry.taskType == taskType || taskType.empty())
            result.push_back(p.second.entry);
    }
    for (const auto& p : embedders_) {
        if (p.second.entry.taskType == taskType || taskType.empty())
            result.push_back(p.second.entry);
    }
    return result;
}

bool ModelRegistry::reloadDetector(const std::string& id, DetectorFactory factory) {
    auto it = detectors_.find(id);
    if (it == detectors_.end()) return false;
    it->second.factory = std::move(factory);
    return true;
}

bool ModelRegistry::reloadEmbedder(const std::string& id, EmbedderFactory factory) {
    auto it = embedders_.find(id);
    if (it == embedders_.end()) return false;
    it->second.factory = std::move(factory);
    return true;
}
