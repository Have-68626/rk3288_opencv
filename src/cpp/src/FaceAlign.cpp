#include "FaceAlign.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {

static cv::Rect clampRectToImage(const cv::Rect2f& r, const cv::Size& sz) {
    if (sz.width <= 0 || sz.height <= 0) return {};
    if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.width) || !std::isfinite(r.height)) return {};
    float x = std::max(0.0f, r.x);
    float y = std::max(0.0f, r.y);
    float w = std::max(0.0f, r.width);
    float h = std::max(0.0f, r.height);
    float x2 = std::min(static_cast<float>(sz.width), x + w);
    float y2 = std::min(static_cast<float>(sz.height), y + h);
    x = std::min(x, x2);
    y = std::min(y, y2);

    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const int ix2 = static_cast<int>(std::ceil(x2));
    const int iy2 = static_cast<int>(std::ceil(y2));
    const int iw = std::max(0, std::min(ix2, sz.width) - std::max(0, ix));
    const int ih = std::max(0, std::min(iy2, sz.height) - std::max(0, iy));
    return cv::Rect(std::max(0, ix), std::max(0, iy), iw, ih);
}

static cv::Mat cropAndResize(const cv::Mat& bgr, const cv::Rect& roi, int outW, int outH) {
    if (bgr.empty()) return {};
    if (roi.width <= 0 || roi.height <= 0) return {};
    if (outW <= 0 || outH <= 0) return {};

    // Performance optimization: Avoid redundant clone() since cv::resize can read directly from a sub-matrix (ROI).
    // Impact: Reduces memory allocation and memory copy overhead per face crop.
    // Rollback: Revert to `cv::Mat cropped = bgr(roi).clone();` if compatibility issues arise with non-continuous data on specific OpenCV versions.
    cv::Mat cropped = bgr(roi);
    if (cropped.empty()) return {};

    cv::Mat resized;
    cv::resize(cropped, resized, cv::Size(outW, outH), 0, 0, cv::INTER_LINEAR);
    return resized;
}

// ArcFace 5 点模板所在的参考坐标系边长：下列模板坐标定义在 112×112 画布上。
constexpr float kArcFaceRefDim = 112.0f;

static std::array<cv::Point2f, 5> arcFace112Ref5() {
    return {cv::Point2f(38.2946f, 51.6963f),
            cv::Point2f(73.5318f, 51.5014f),
            cv::Point2f(56.0252f, 71.7366f),
            cv::Point2f(41.5493f, 92.3655f),
            cv::Point2f(70.7299f, 92.2041f)};
}

static bool pointsFinite(const std::array<cv::Point2f, 5>& pts) {
    for (const auto& p : pts) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    }
    return true;
}

}  // namespace

FaceAlignResult alignFaceForArcFace112(const cv::Mat& bgr, const FaceDetection& det, const FaceAlignOptions& opt) {
    FaceAlignResult r;
    if (bgr.empty()) {
        r.err = "FaceAlign: 输入图像为空";
        return r;
    }
    if (opt.outW <= 0 || opt.outH <= 0) {
        r.err = "FaceAlign: 输出尺寸非法";
        return r;
    }

    if (opt.preferKeypoints5 && det.keypoints5.has_value()) {
        const auto& kps = *det.keypoints5;
        if (pointsFinite(kps)) {
            std::array<cv::Point2f, 5> src;
            std::array<cv::Point2f, 5> dst;
            for (int i = 0; i < 5; i++) src[static_cast<size_t>(i)] = kps[static_cast<size_t>(i)];
            const auto ref = arcFace112Ref5();
            // 参考点定义在 112×112 坐标系，而下面的 warpAffine 渲染到 opt.outW×outH —— 必须先把
            // 参考点按输出画布**等比缩放**，否则人脸会以 **112 尺度**落在画布**左上角**
            // （outW>112 时右/下留黑边、outW<112 时被裁），与 bbox 回退路径「裁剪后缩放填满画布」不一致。
            //
            // ⚠️ 为什么取**单轴**比例（min(outW,outH)/112），而不是各轴独立的 sx=outW/112、sy=outH/112：
            //    `cv::estimateAffinePartial2D` 求的是**相似变换（4 自由度：旋转 + 等比缩放 + 平移）**，
            //    它**无法表达各轴独立缩放**；传入各轴不同的目标只会得到**最小二乘折中**。
            //    实测证据（本文件对应用例）：160×96 下把目标设为各轴独立时，源图中的正方形特征被
            //    渲染成 **76×75（长宽比 1.013）**而非预期的 1.667 —— 尺度不可解释，且可能裁掉人脸。
            //    取 min 的语义是明确的：等比缩放到**完整容纳**人脸，不裁剪。
            // ⚠️ 与 bbox 回退路径在**非方画布**下无法完全对称（后者是各轴独立的 cv::resize）——
            //    这是相似变换的能力边界，非疏漏。两个随包 manifest 均为 112×112（方画布），
            //    该情形下两者语义一致；方画布也是本项的实际场景。
            // outW=outH=112 时 s=1 ⇒ 与既有行为逐比特一致，零回归。
            const float s = std::min(static_cast<float>(opt.outW), static_cast<float>(opt.outH)) /
                            kArcFaceRefDim;
            for (int i = 0; i < 5; i++) {
                const auto& p = ref[static_cast<size_t>(i)];
                dst[static_cast<size_t>(i)] = cv::Point2f(p.x * s, p.y * s);
            }

            cv::Mat M = cv::estimateAffinePartial2D(src, dst, cv::noArray(), cv::LMEDS);
            if (!M.empty() && M.cols == 3 && M.rows == 2) {
                cv::Mat aligned;
                cv::warpAffine(bgr, aligned, M, cv::Size(opt.outW, opt.outH), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
                if (!aligned.empty()) {
                    r.alignedBgr = std::move(aligned);
                    r.usedKeypoints5 = true;
                    return r;
                }
            }
        }
    }

    const cv::Rect roi = clampRectToImage(det.bbox, bgr.size());
    if (roi.width <= 0 || roi.height <= 0) {
        r.err = "FaceAlign: bbox 越界或为空";
        return r;
    }

    cv::Mat aligned = cropAndResize(bgr, roi, opt.outW, opt.outH);
    if (aligned.empty()) {
        r.err = "FaceAlign: bbox 裁剪/resize 失败";
        return r;
    }
    r.alignedBgr = std::move(aligned);
    r.usedBboxFallback = true;
    return r;
}
