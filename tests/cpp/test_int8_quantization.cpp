#include "ModelRegistry.h"

#include <fstream>
#include <string>

namespace {

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

}  // namespace

static bool skipIfNoModel(const std::string& path) {
    if (!fileExists(path)) return true;
    return false;
}

bool test_int8_yolo_face_registered() {
    ModelRegistry::ensureBuiltinRegistered();
    if (skipIfNoModel("models/yolo_face_int8_ncnn/yolo_face_int8.param")) return true;
    auto* entry = ModelRegistry::instance().getEntry("yolo_face_int8");
    return entry != nullptr && entry->taskType == "detect";
}

bool test_int8_arcface_registered() {
    ModelRegistry::ensureBuiltinRegistered();
    if (skipIfNoModel("models/arcface_int8_ncnn/arcface_int8.param")) return true;
    auto* entry = ModelRegistry::instance().getEntry("arcface_int8");
    return entry != nullptr && entry->taskType == "recognize";
}

bool test_int8_mobilefacenet_registered() {
    ModelRegistry::ensureBuiltinRegistered();
    if (skipIfNoModel("models/mobilefacenet_int8_ncnn/mobilefacenet_int8.param")) return true;
    auto* entry = ModelRegistry::instance().getEntry("mobilefacenet_int8");
    return entry != nullptr && entry->taskType == "recognize";
}

bool test_int8_yolo_face_creates_detector() {
    ModelRegistry::ensureBuiltinRegistered();
    if (skipIfNoModel("models/yolo_face_int8_ncnn/yolo_face_int8.param")) return true;
    std::string err;
    auto det = ModelRegistry::instance().createDetector("yolo_face_int8", &err);
    return det != nullptr;
}

bool test_int8_arcface_creates_embedder() {
    ModelRegistry::ensureBuiltinRegistered();
    if (skipIfNoModel("models/arcface_int8_ncnn/arcface_int8.param")) return true;
    std::string err;
    auto emb = ModelRegistry::instance().createEmbedder("arcface_int8", &err);
    return emb != nullptr;
}

bool test_int8_mobilefacenet_creates_embedder() {
    ModelRegistry::ensureBuiltinRegistered();
    if (skipIfNoModel("models/mobilefacenet_int8_ncnn/mobilefacenet_int8.param")) return true;
    std::string err;
    auto emb = ModelRegistry::instance().createEmbedder("mobilefacenet_int8", &err);
    return emb != nullptr;
}
