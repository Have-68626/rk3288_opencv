#include "ModelRegistry.h"

#include <fstream>
#include <string>

namespace {

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

bool int8ModelsAvailable() {
    return fileExists("models/yolo_face_int8_ncnn/yolo_face_int8.param") ||
           fileExists("models/arcface_int8_ncnn/arcface_int8.param") ||
           fileExists("models/mobilefacenet_int8_ncnn/mobilefacenet_int8.param");
}

}  // namespace

bool test_int8_yolo_face_registered() {
    ModelRegistry::ensureBuiltinRegistered();
    if (!int8ModelsAvailable()) return true;  // skip
    auto* entry = ModelRegistry::instance().getEntry("yolo_face_int8");
    return entry != nullptr && entry->taskType == "detect";
}

bool test_int8_arcface_registered() {
    ModelRegistry::ensureBuiltinRegistered();
    if (!int8ModelsAvailable()) return true;  // skip
    auto* entry = ModelRegistry::instance().getEntry("arcface_int8");
    return entry != nullptr && entry->taskType == "recognize";
}

bool test_int8_mobilefacenet_registered() {
    ModelRegistry::ensureBuiltinRegistered();
    if (!int8ModelsAvailable()) return true;  // skip
    auto* entry = ModelRegistry::instance().getEntry("mobilefacenet_int8");
    return entry != nullptr && entry->taskType == "recognize";
}

bool test_int8_yolo_face_creates_detector() {
    ModelRegistry::ensureBuiltinRegistered();
    if (!int8ModelsAvailable()) return true;  // skip
    std::string err;
    auto det = ModelRegistry::instance().createDetector("yolo_face_int8", &err);
    return det != nullptr;
}

bool test_int8_arcface_creates_embedder() {
    ModelRegistry::ensureBuiltinRegistered();
    if (!int8ModelsAvailable()) return true;  // skip
    std::string err;
    auto emb = ModelRegistry::instance().createEmbedder("arcface_int8", &err);
    return emb != nullptr;
}

bool test_int8_mobilefacenet_creates_embedder() {
    ModelRegistry::ensureBuiltinRegistered();
    if (!int8ModelsAvailable()) return true;  // skip
    std::string err;
    auto emb = ModelRegistry::instance().createEmbedder("mobilefacenet_int8", &err);
    return emb != nullptr;
}
