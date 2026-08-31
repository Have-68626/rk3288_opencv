#include "pipeline/FacePostProcessor.h"

#include <algorithm>
#include <cmath>

namespace pipeline {

namespace {

double rectArea(const cv::Rect& r) {
    return std::max(0, r.width) * std::max(0, r.height);
}

// 矩形交集：不用 cv::Rect operator&，因 RK_SKIP_OPENCV 的 mock Rect 未实现该运算符
cv::Rect intersectRect(const cv::Rect& a, const cv::Rect& b) {
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);
    return cv::Rect(x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1));
}

double iou(const cv::Rect& a, const cv::Rect& b) {
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);
    const int iw = std::max(0, x2 - x1);
    const int ih = std::max(0, y2 - y1);
    const double inter = static_cast<double>(iw) * ih;
    const double uni = rectArea(a) + rectArea(b) - inter;
    if (uni <= 0.0) return 0.0;
    return inter / uni;
}

// inner 被 outer 包含的面积占比（inner ∩ outer / inner 面积）
double containedRatio(const cv::Rect& inner, const cv::Rect& outer) {
    const double inArea = rectArea(inner);
    if (inArea <= 0.0) return 0.0;
    const int x1 = std::max(inner.x, outer.x);
    const int y1 = std::max(inner.y, outer.y);
    const int x2 = std::min(inner.x + inner.width, outer.x + outer.width);
    const int y2 = std::min(inner.y + inner.height, outer.y + outer.height);
    const int iw = std::max(0, x2 - x1);
    const int ih = std::max(0, y2 - y1);
    return (static_cast<double>(iw) * ih) / inArea;
}

}  // namespace

std::vector<std::size_t> FacePostProcessor::keptIndices(
    const std::vector<DetectedFace>& faces,
    int frameWidth, int frameHeight,
    const FacePostProcessOptions& opt) {
    const bool clipToFrame = (frameWidth > 0 && frameHeight > 0);
    const cv::Rect bounds(0, 0, std::max(frameWidth, 0), std::max(frameHeight, 0));

    // 1. 丢弃 NaN 置信度、负尺寸、越界后无有效面积的框；2. 裁剪到画面边界
    std::vector<std::size_t> order;            // 有效检测在 faces 中的原始索引（加入序 = 原始序）
    std::vector<DetectedFace> clippedFaces;    // 与 order 平行的裁剪后数据
    order.reserve(faces.size());
    clippedFaces.reserve(faces.size());
    for (std::size_t i = 0; i < faces.size(); i++) {
        const DetectedFace& f = faces[i];
        if (std::isnan(f.confidence)) continue;
        if (f.bbox.width <= 0 || f.bbox.height <= 0) continue;
        DetectedFace c = f;
        if (clipToFrame) {
            cv::Rect clipped = intersectRect(f.bbox, bounds);
            if (clipped.width <= 0 || clipped.height <= 0) continue;
            c.bbox = clipped;
        }
        order.push_back(i);
        clippedFaces.push_back(std::move(c));
    }
    if (order.empty()) return order;

    // 3. 稳定排序：置信度降序；同分数 stable_sort 保持原始次序（== 原始索引升序）
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return clippedFaces[a].confidence > clippedFaces[b].confidence;
    });

    // 4. IoU NMS（贪心，最高分先选）
    std::vector<char> nmsSuppressed(order.size(), 0);
    for (std::size_t ii = 0; ii < order.size(); ii++) {
        if (nmsSuppressed[ii]) continue;
        for (std::size_t jj = ii + 1; jj < order.size(); jj++) {
            if (nmsSuppressed[jj]) continue;
            if (iou(clippedFaces[order[ii]].bbox, clippedFaces[order[jj]].bbox) >
                opt.nmsIouThreshold) {
                nmsSuppressed[jj] = 1;
            }
        }
    }
    std::vector<std::size_t> kept;  // 原始索引，已按排序输出序排列
    kept.reserve(order.size());
    for (std::size_t ii = 0; ii < order.size(); ii++) {
        if (!nmsSuppressed[ii]) kept.push_back(order[ii]);
    }

    // 5. 包含框抑制：较小框 ≥ containmentRatio 被较大框包含且分数不更高时删除较小框
    std::vector<char> containSuppressed(kept.size(), 0);
    for (std::size_t i = 0; i < kept.size(); i++) {
        if (containSuppressed[i]) continue;
        for (std::size_t j = i + 1; j < kept.size(); j++) {
            if (containSuppressed[j]) continue;
            const cv::Rect& a = clippedFaces[kept[i]].bbox;
            const cv::Rect& b = clippedFaces[kept[j]].bbox;
            const bool aSmaller = rectArea(a) <= rectArea(b);
            const cv::Rect& big = aSmaller ? b : a;
            const cv::Rect& small = aSmaller ? a : b;
            if (containedRatio(small, big) < opt.containmentRatio) continue;
            if (aSmaller && clippedFaces[kept[i]].confidence <= clippedFaces[kept[j]].confidence) {
                containSuppressed[i] = 1;
            } else if (!aSmaller && clippedFaces[kept[j]].confidence <= clippedFaces[kept[i]].confidence) {
                containSuppressed[j] = 1;
            }
        }
    }
    std::vector<std::size_t> out;
    out.reserve(kept.size());
    for (std::size_t i = 0; i < kept.size(); i++) {
        if (!containSuppressed[i]) out.push_back(kept[i]);
    }

    // 6. maxFaces 截断（已保持稳定排序顺序）
    if (opt.maxFaces > 0 && static_cast<int>(out.size()) > opt.maxFaces) {
        out.resize(static_cast<std::size_t>(opt.maxFaces));
    }
    return out;
}

std::vector<DetectedFace> FacePostProcessor::process(
    std::vector<DetectedFace> faces,
    int frameWidth, int frameHeight,
    const FacePostProcessOptions& opt) {
    std::vector<std::size_t> idx = keptIndices(faces, frameWidth, frameHeight, opt);
    const bool clipToFrame = (frameWidth > 0 && frameHeight > 0);
    const cv::Rect bounds(0, 0, std::max(frameWidth, 0), std::max(frameHeight, 0));
    std::vector<DetectedFace> out;
    out.reserve(idx.size());
    for (std::size_t k : idx) {
        DetectedFace f = faces[k];
        if (clipToFrame) f.bbox = intersectRect(f.bbox, bounds);  // keptIndices 已确保该项与画面有交
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace pipeline