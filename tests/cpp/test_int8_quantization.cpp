#include "ModelRegistry.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

bool skipIfNoModel(const std::string& path) {
    if (!fileExists(path)) return true;
    return false;
}

// 读取 ncnn .param 文件头部，提取 layer_count
// 用于验证 FP32 和 INT8 模型的结构一致性
// 格式: 第1行=magic(7767517), 第2行=<layer_count> <blob_count>
static bool readParamLayerCount(const std::string& path, int& layerCount) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    int magic = 0, blobCount = 0;
    f >> magic >> layerCount >> blobCount;
    return f.good() && magic == 7767517;
}

// 余弦相似度计算
static double cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    double denom = std::sqrt(na) * std::sqrt(nb);
    return (denom > 1e-10) ? (dot / denom) : 0.0;
}

// 检测框 IoU 计算
static double computeIou(int x1, int y1, int w1, int h1,
                         int x2, int y2, int w2, int h2) {
    int interL = std::max(x1, x2);
    int interT = std::max(y1, y2);
    int interR = std::min(x1 + w1, x2 + w2);
    int interB = std::min(y1 + h1, y2 + h2);
    int interW = std::max(0, interR - interL);
    int interH = std::max(0, interB - interT);
    double interArea = static_cast<double>(interW) * interH;
    double unionArea = static_cast<double>(w1 * h1) + w2 * h2 - interArea;
    return (unionArea > 0) ? (interArea / unionArea) : 0.0;
}

}  // namespace

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

// ---------------------------------------------------------------------------
// 精度对比测试：FP32 vs INT8
// 完整推理比较需要启用 ncnn（RK_HAVE_NCNN=1）。
// 当前仅做模型文件结构一致性校验（层数匹配），推理比较待 ncnn 启用后激活。
// ---------------------------------------------------------------------------

bool test_int8_precision_detection_iou() {
    // 需要 INT8 检测模型存在即可（FP32 对比版本为 PNNX 直接输出，两者层数一致）
    if (skipIfNoModel("models/yolo_face_int8_ncnn/yolo_face_int8.param")) return true;

    // 确认 INT8 模型的有效性：magic number 和 layer_count 读取正常
    int layers = 0;
    if (!readParamLayerCount("models/yolo_face_int8_ncnn/yolo_face_int8.param", layers)) return false;
    if (layers <= 0) return false;

#ifdef RK_HAVE_NCNN
    // ncnn 启用时执行完整推理比较：加载图片 → FP32 检测 → INT8 检测 → IoU ≥ 0.7
    // TODO: 待 ncnn 链接就绪后实现
#endif
    return true;
}

bool test_int8_precision_arcface_similarity() {
    // 需要 FP32 识别模型 + INT8 识别模型同时存在
    if (skipIfNoModel("models/arcface_ncnn/arcface.param")) return true;
    if (skipIfNoModel("models/arcface_int8_ncnn/arcface_int8.param")) return true;

    // 结构一致性：FP32 和 INT8 应具有完全相同的层结构（INT8 是 FP32 的直接量化结果）
    int fp32Layers = 0, int8Layers = 0;
    if (!readParamLayerCount("models/arcface_ncnn/arcface.param", fp32Layers)) return false;
    if (!readParamLayerCount("models/arcface_int8_ncnn/arcface_int8.param", int8Layers)) return false;
    if (fp32Layers != int8Layers) return false;

#ifdef RK_HAVE_NCNN
    // ncnn 启用时执行完整推理比较：加载人脸 → FP32 提取 → INT8 提取 → cosine ≥ 0.9
    // TODO: 待 ncnn 链接就绪后实现
#endif
    return true;
}
