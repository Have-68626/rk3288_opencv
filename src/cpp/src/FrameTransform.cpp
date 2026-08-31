#include "FrameTransform.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

namespace rk_core {

// 水平实际翻转 = input XOR target
bool FrameTransform::effectiveFlipX(const FrameTransformConfig& c) {
    return c.input_mirrored_x != c.target_mirrored_x;
}

// 排除 generation（它是派生计数，不参与"配置是否变化"判别）
bool FrameTransform::configUnchanged(const FrameTransformConfig& a, const FrameTransformConfig& b) {
    return a.rotation_degrees == b.rotation_degrees &&
           a.input_mirrored_x == b.input_mirrored_x &&
           a.target_mirrored_x == b.target_mirrored_x &&
           a.flip_y == b.flip_y;
}

// 先旋转后翻转；翻转出来的是新 Mat，输入 frame 不被修改
cv::Mat FrameTransform::applyImage(const cv::Mat& frame, const FrameTransformConfig& c) {
    int rot = ((c.rotation_degrees % 360) + 360) % 360;

    cv::Mat rotated;
    switch (rot) {
        case 90:
            cv::rotate(frame, rotated, cv::ROTATE_90_CLOCKWISE);
            break;
        case 180:
            cv::rotate(frame, rotated, cv::ROTATE_180);
            break;
        case 270:
            cv::rotate(frame, rotated, cv::ROTATE_90_COUNTERCLOCKWISE);
            break;
        default:
            rotated = frame;
            break;
    }

    const bool fx = effectiveFlipX(c);
    if (!fx && !c.flip_y) {
        return rotated;
    }
    const int code = (fx && c.flip_y) ? -1 : (c.flip_y ? 0 : 1);
    cv::Mat flipped;
    cv::flip(rotated, flipped, code);
    return flipped;
}

// 正向映射：输入坐标 -> applyImage 输出坐标（旋转后翻转）
cv::Point2f FrameTransform::mapPoint(const cv::Point2f& p, const FrameTransformConfig& c,
                                     int inWidth, int inHeight) {
    const int rot = ((c.rotation_degrees % 360) + 360) % 360;
    float x = p.x;
    float y = p.y;
    if (rot == 90) {
        const float t = x;
        x = static_cast<float>(inHeight - 1) - y;
        y = t;
    } else if (rot == 180) {
        x = static_cast<float>(inWidth - 1) - x;
        y = static_cast<float>(inHeight - 1) - y;
    } else if (rot == 270) {
        const float t = y;
        y = static_cast<float>(inWidth - 1) - x;
        x = t;
    }

    const bool swapped = (rot == 90 || rot == 270);
    const int outW = swapped ? inHeight : inWidth;
    const int outH = swapped ? inWidth : inHeight;
    if (effectiveFlipX(c)) {
        x = static_cast<float>(outW - 1) - x;
    }
    if (c.flip_y) {
        y = static_cast<float>(outH - 1) - y;
    }
    return {x, y};
}

// 映射矩形四角后取外接框
cv::Rect FrameTransform::mapRect(const cv::Rect& r, const FrameTransformConfig& c,
                                 int inWidth, int inHeight) {
    const cv::Point2f pts[4] = {
        mapPoint({static_cast<float>(r.x), static_cast<float>(r.y)}, c, inWidth, inHeight),
        mapPoint({static_cast<float>(r.x + r.width), static_cast<float>(r.y)}, c, inWidth, inHeight),
        mapPoint({static_cast<float>(r.x), static_cast<float>(r.y + r.height)}, c, inWidth, inHeight),
        mapPoint({static_cast<float>(r.x + r.width), static_cast<float>(r.y + r.height)}, c, inWidth, inHeight),
    };
    float x0 = pts[0].x, x1 = pts[0].x;
    float y0 = pts[0].y, y1 = pts[0].y;
    for (int i = 1; i < 4; i++) {
        x0 = std::min(x0, pts[i].x);
        x1 = std::max(x1, pts[i].x);
        y0 = std::min(y0, pts[i].y);
        y1 = std::max(y1, pts[i].y);
    }
    const int rx = static_cast<int>(std::floor(x0));
    const int ry = static_cast<int>(std::floor(y0));
    const int rw = static_cast<int>(std::ceil(x1)) - rx;
    const int rh = static_cast<int>(std::ceil(y1)) - ry;
    return cv::Rect(rx, ry, rw, rh);
}

}  // namespace rk_core