#include "pipeline/FacePostProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pipeline {

namespace {

// ★ 2026-09-28 修（外部审查 [P2]「异常检测框可触发整数转换或加法溢出」+ 独立复核）：
//   本文件原先所有矩形端点运算都在 **int** 域进行：`a.x + a.width`（三处）、
//   `rectArea` 的 `w * h`（int×int）。`cv::Rect` 的 x/width 是 int，而**生产者**一侧
//   （`Engine::postProcessRealtimeBoxes` / `ScrfdDecoder`）在修复前可以把接近 int 边界的端点
//   写进 bbox ⇒ 这些加/减/乘都是有符号溢出 UB。
//   修法：**端点一律在 `long long` 域算**，只在写回 `cv::Rect` 时收敛回 int 域并夹取。
//   行为变化：对合法输入**零变化**（合法输入的端点差远小于 int 域，夹取不触发）。
//   注意：本文件被编入 `core_unit_tests`（mock OpenCV）与 `core_gtest_tests` 两侧，
//   `long long` 算术对两者都成立（不依赖 OpenCV）。
constexpr long long kRectMax = static_cast<long long>(std::numeric_limits<int>::max());

double rectArea(const cv::Rect& r) {
    const long long w = std::max<long long>(0, r.width);
    const long long h = std::max<long long>(0, r.height);
    return static_cast<double>(w * h);  // 64 位乘 ⇒ 不再有 int×int 溢出
}

int clampRectDim(long long v) {
    return static_cast<int>(std::min<long long>(std::max<long long>(0, v), kRectMax));
}

// 矩形交集：不用 cv::Rect operator&，因 RK_SKIP_OPENCV 的 mock Rect 未实现该运算符
cv::Rect intersectRect(const cv::Rect& a, const cv::Rect& b) {
    const long long ax2 = static_cast<long long>(a.x) + a.width;
    const long long ay2 = static_cast<long long>(a.y) + a.height;
    const long long bx2 = static_cast<long long>(b.x) + b.width;
    const long long by2 = static_cast<long long>(b.y) + b.height;
    const long long x1 = std::max<long long>(a.x, b.x);
    const long long y1 = std::max<long long>(a.y, b.y);
    const long long x2 = std::min(ax2, bx2);
    const long long y2 = std::min(ay2, by2);
    return cv::Rect(static_cast<int>(x1), static_cast<int>(y1),
                    clampRectDim(x2 - x1), clampRectDim(y2 - y1));
}

double iou(const cv::Rect& a, const cv::Rect& b) {
    const long long ax2 = static_cast<long long>(a.x) + a.width;
    const long long ay2 = static_cast<long long>(a.y) + a.height;
    const long long bx2 = static_cast<long long>(b.x) + b.width;
    const long long by2 = static_cast<long long>(b.y) + b.height;
    const long long x1 = std::max<long long>(a.x, b.x);
    const long long y1 = std::max<long long>(a.y, b.y);
    const long long x2 = std::min(ax2, bx2);
    const long long y2 = std::min(ay2, by2);
    const long long iw = std::max<long long>(0, x2 - x1);
    const long long ih = std::max<long long>(0, y2 - y1);
    const double inter = static_cast<double>(iw) * static_cast<double>(ih);
    const double uni = rectArea(a) + rectArea(b) - inter;
    if (uni <= 0.0) return 0.0;
    return inter / uni;
}

// inner 被 outer 包含的面积占比（inner ∩ outer / inner 面积）
double containedRatio(const cv::Rect& inner, const cv::Rect& outer) {
    const double inArea = rectArea(inner);
    if (inArea <= 0.0) return 0.0;
    const long long ix2 = static_cast<long long>(inner.x) + inner.width;
    const long long iy2 = static_cast<long long>(inner.y) + inner.height;
    const long long ox2 = static_cast<long long>(outer.x) + outer.width;
    const long long oy2 = static_cast<long long>(outer.y) + outer.height;
    const long long x1 = std::max<long long>(inner.x, outer.x);
    const long long y1 = std::max<long long>(inner.y, outer.y);
    const long long x2 = std::min(ix2, ox2);
    const long long y2 = std::min(iy2, oy2);
    const long long iw = std::max<long long>(0, x2 - x1);
    const long long ih = std::max<long long>(0, y2 - y1);
    return (static_cast<double>(iw) * static_cast<double>(ih)) / inArea;
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
    // 与 faces 等长，下标 == faces 原始索引；无效槽位保持默认构造且永不被 order 引用。
    // 关键：order 存的是原始索引，若 clippedFaces 用 push_back 紧凑存放，则后续
    // clippedFaces[order[...]] 会用原始索引越界访问紧凑数组（丢弃无效框后索引错位）。
    std::vector<DetectedFace> clippedFaces(faces.size());
    order.reserve(faces.size());
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
        clippedFaces[i] = std::move(c);  // 以原始索引写入，保证 order 元素可直接寻址
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