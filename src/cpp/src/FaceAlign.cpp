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
    cv::Mat cropped = bgr(roi).clone();
    if (cropped.empty()) return {};
    cv::Mat resized;
    cv::resize(cropped, resized, cv::Size(outW, outH), 0, 0, cv::INTER_LINEAR);
    return resized;
}

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

    // 关键路径说明：
    // - 若有 5 点关键点，优先做仿射对齐到 ArcFace 112x112 参考点（口径固定，便于后续 Android/服务端一致）。
    // - 关键点不可用/估计失败时，退化为 bbox 裁剪 + resize，保证闭环不断。
    if (opt.preferKeypoints5 && det.keypoints5.has_value()) {
        const auto& kps = *det.keypoints5;
        if (pointsFinite(kps)) {
            std::vector<cv::Point2f> src;
            std::vector<cv::Point2f> dst;
            src.reserve(5);
            dst.reserve(5);
            for (int i = 0; i < 5; i++) src.push_back(kps[static_cast<size_t>(i)]);
            const auto ref = arcFace112Ref5();
            for (int i = 0; i < 5; i++) dst.push_back(ref[static_cast<size_t>(i)]);

            cv::Mat inliers;
            cv::Mat M = cv::estimateAffinePartial2D(src, dst, inliers, cv::LMEDS);
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
