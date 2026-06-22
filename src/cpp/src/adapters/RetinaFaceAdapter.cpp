#include "adapters/RetinaFaceAdapter.h"

#include <algorithm>
#include <cmath>
#include <set>

RetinaFaceAdapter::RetinaFaceAdapter() {
    // SCRFD/det_10g FPN anchor 配置：stride 8/16/32
    anchorCfgs_ = {
        {8,  {16, 32},    {1.0f}, {1.0f, 0.5f, 2.0f}},
        {16, {64, 128},   {1.0f}, {1.0f, 0.5f, 2.0f}},
        {32, {256, 512},  {1.0f}, {1.0f, 0.5f, 2.0f}},
    };
}

RetinaFaceAdapter::~RetinaFaceAdapter() = default;

bool RetinaFaceAdapter::load(const std::string& modelPath, std::string& err) {
    loaded_ = false;
    net_ = cv::dnn::readNet(modelPath);
    if (net_.empty()) {
        err = "RetinaFaceAdapter: readNet failed for " + modelPath;
        return false;
    }
    net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);

    // 运行时发现输出层名
    outputNames_ = net_.getUnconnectedOutLayersNames();
    if (outputNames_.empty()) {
        err = "RetinaFaceAdapter: no output layers found";
        return false;
    }

    currentName_ = "retinaface_scrfd";
    loaded_ = true;
    return true;
}

static float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

void RetinaFaceAdapter::generateAnchors(int stride, const std::vector<int>& baseSize,
                                         const std::vector<float>& ratios,
                                         const std::vector<float>& scales,
                                         int featW, int featH,
                                         std::vector<std::vector<float>>& anchors) {
    anchors.clear();
    for (int i = 0; i < featH; i++) {
        for (int j = 0; j < featW; j++) {
            float cx = (j + 0.5f) * stride;
            float cy = (i + 0.5f) * stride;
            for (int b : baseSize) {
                for (float r : ratios) {
                    float sr = std::sqrt(r);
                    float baseW = b / sr;
                    float baseH = b * sr;
                    for (float s : scales) {
                        float w = baseW * s;
                        float h = baseH * s;
                        anchors.push_back({cx, cy, w, h});
                    }
                }
            }
        }
    }
}

float RetinaFaceAdapter::intersectArea(float ax1, float ay1, float ax2, float ay2,
                                        float bx1, float by1, float bx2, float by2) {
    float ix1 = std::max(ax1, bx1);
    float iy1 = std::max(ay1, by1);
    float ix2 = std::min(ax2, bx2);
    float iy2 = std::min(ay2, by2);
    float iw = std::max(0.0f, ix2 - ix1 + 1);
    float ih = std::max(0.0f, iy2 - iy1 + 1);
    return iw * ih;
}

void RetinaFaceAdapter::nms(std::vector<FaceDetection>& dets, float threshold) {
    std::sort(dets.begin(), dets.end(), [](const FaceDetection& a, const FaceDetection& b) {
        return a.score > b.score;
    });
    std::vector<bool> keep(dets.size(), true);
    for (size_t i = 0; i < dets.size(); i++) {
        if (!keep[i]) continue;
        for (size_t j = i + 1; j < dets.size(); j++) {
            if (!keep[j]) continue;
            float ax1 = dets[i].bbox.x;
            float ay1 = dets[i].bbox.y;
            float ax2 = ax1 + dets[i].bbox.width;
            float ay2 = ay1 + dets[i].bbox.height;
            float bx1 = dets[j].bbox.x;
            float by1 = dets[j].bbox.y;
            float bx2 = bx1 + dets[j].bbox.width;
            float by2 = by1 + dets[j].bbox.height;
            float inter = intersectArea(ax1, ay1, ax2, ay2, bx1, by1, bx2, by2);
            float areaA = dets[i].bbox.width * dets[i].bbox.height;
            float areaB = dets[j].bbox.width * dets[j].bbox.height;
            float iou = inter / (areaA + areaB - inter);
            if (iou > threshold) keep[j] = false;
        }
    }
    std::vector<FaceDetection> out;
    for (size_t i = 0; i < dets.size(); i++) {
        if (keep[i]) out.push_back(dets[i]);
    }
    dets.swap(out);
}

FaceDetections RetinaFaceAdapter::detect(const cv::Mat& bgr, std::string& err) {
    if (!loaded_) {
        err = "retinaface_not_loaded";
        return {};
    }

    cv::Mat blob = cv::dnn::blobFromImage(bgr, 1.0 / 255.0,
        cv::Size(inputW_, inputH_),
        cv::Scalar(0, 0, 0), true, false);
    net_.setInput(blob);

    std::vector<cv::Mat> outputs;
    net_.forward(outputs, outputNames_);

    if (outputs.empty()) {
        err = "RetinaFaceAdapter: forward returned no outputs";
        return {};
    }

    // 尝试按 SCRFD 命名约定解析：score_XX / bbox_XX / kps_XX
    // 若输出名不匹配，尝试按输出索引顺序：score / bbox / kps 成组
    struct FpnLevel {
        int stride;
        const float* score;
        const float* bbox;
        int channels;
        int featW;
        int featH;
    };
    std::vector<FpnLevel> levels;

    for (size_t i = 0; i < outputNames_.size(); i++) {
        const auto& name = outputNames_[i];
        auto& mat = outputs[i];
        if (mat.empty()) continue;

        int total = mat.total();
        int ch = mat.size[1];
        int h = mat.size[2];
        int w = mat.size[3];

        // 尝试从输出名提取 stride
        int stride = 0;
        for (const auto& ac : anchorCfgs_) {
            if (name.find(std::to_string(ac.stride)) != std::string::npos) {
                stride = ac.stride;
                break;
            }
        }
        if (stride == 0) {
            // 若无法从名称提取，按特征图尺寸推断 stride
            stride = inputW_ / w;
        }

        if (name.find("score") != std::string::npos || name.find("cls") != std::string::npos) {
            // score 输出，记录
            auto it = std::find_if(levels.begin(), levels.end(), [stride](const FpnLevel& l) {
                return l.stride == stride;
            });
            if (it != levels.end()) {
                it->score = mat.ptr<float>();
            } else {
                levels.push_back({stride, mat.ptr<float>(), nullptr, ch, w, h});
            }
        } else if (name.find("bbox") != std::string::npos || name.find("box") != std::string::npos) {
            auto it = std::find_if(levels.begin(), levels.end(), [stride](const FpnLevel& l) {
                return l.stride == stride;
            });
            if (it != levels.end()) {
                it->bbox = mat.ptr<float>();
            } else {
                levels.push_back({stride, nullptr, mat.ptr<float>(), ch, w, h});
            }
        }
    }

    // 若命名解析失败，尝试按顺序：假设输出按 score0,bbox0,score1,bbox1,... 排列
    if (levels.empty()) {
        for (size_t i = 0; i + 1 < outputs.size(); i += 2) {
            int total = outputs[i].total();
            int h = outputs[i].size[2];
            int w = outputs[i].size[3];
            int stride = inputW_ / w;
            levels.push_back({stride, outputs[i].ptr<float>(),
                              outputs[i + 1].ptr<float>(),
                              outputs[i].size[1], w, h});
        }
    }

    // 解码检测框
    std::vector<FaceDetection> allDets;
    for (const auto& level : levels) {
        if (!level.score || !level.bbox) continue;

        int stride = level.stride;
        auto it = std::find_if(anchorCfgs_.begin(), anchorCfgs_.end(),
            [stride](const AnchorCfg& ac) { return ac.stride == stride; });
        if (it == anchorCfgs_.end()) continue;

        std::vector<std::vector<float>> anchors;
        generateAnchors(it->stride, it->baseSize, it->ratios, it->scales,
                        level.featW, level.featH, anchors);

        int numAnchors = (int)anchors.size();
        int scoreCh = level.channels;  // numAnchors × 2 (bg/fg) 或 numAnchors × 1 (sigmoid)
        int bboxCh = level.channels;
        int anchorsPerPos = numAnchors / (level.featW * level.featH);

        for (int a = 0; a < numAnchors; a++) {
            float s = level.score[a * 2 + 1];  // 假设 score 输出为 [bg, fg] 格式
            if (s < confThreshold_) continue;

            float dx = level.bbox[a * 4 + 0];
            float dy = level.bbox[a * 4 + 1];
            float dw = level.bbox[a * 4 + 2];
            float dh = level.bbox[a * 4 + 3];

            float cx = anchors[a][0] + dx * it->stride;
            float cy = anchors[a][1] + dy * it->stride;
            float bw = anchors[a][2] * std::exp(dw);
            float bh = anchors[a][3] * std::exp(dh);

            FaceDetection d;
            d.bbox = cv::Rect2f(cx - bw / 2, cy - bh / 2, bw, bh);
            d.score = s;
            allDets.push_back(std::move(d));
        }
    }

    nms(allDets, nmsThreshold_);

    return allDets;
}

const char* RetinaFaceAdapter::name() const {
    return currentName_.c_str();
}
