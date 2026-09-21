#include "FaceAlign.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

bool test_face_align_basic() {
    cv::Mat img(480, 640, CV_8UC3, cv::Scalar(128, 128, 128));

    FaceDetection det;
    det.bbox = cv::Rect2f(200.0f, 100.0f, 100.0f, 120.0f);

    FaceAlignOptions opt;
    opt.outW = 112;
    opt.outH = 112;

    auto r = alignFaceForArcFace112(img, det, opt);
    if (r.alignedBgr.empty()) { std::fprintf(stderr, "alignedBgr empty\n"); return false; }
    if (r.alignedBgr.cols != 112 || r.alignedBgr.rows != 112) { std::fprintf(stderr, "wrong size\n"); return false; }
    if (!r.usedBboxFallback) { std::fprintf(stderr, "expected bbox fallback\n"); return false; }
    return true;
}

bool test_face_align_nan_bbox() {
    cv::Mat img(480, 640, CV_8UC3, cv::Scalar(128, 128, 128));

    FaceDetection det;
    det.bbox = cv::Rect2f(NAN, NAN, NAN, NAN);

    FaceAlignOptions opt;
    opt.outW = 112;
    opt.outH = 112;

    auto r = alignFaceForArcFace112(img, det, opt);
    if (!r.alignedBgr.empty()) { std::fprintf(stderr, "expected empty result for NaN bbox\n"); return false; }
    if (r.err.empty()) { std::fprintf(stderr, "expected error message\n"); return false; }
    return true;
}

bool test_face_align_empty_image() {
    cv::Mat empty;
    FaceDetection det;
    det.bbox = cv::Rect2f(0, 0, 100, 100);
    FaceAlignOptions opt;
    auto r = alignFaceForArcFace112(empty, det, opt);
    if (!r.alignedBgr.empty()) return false;
    return true;
}

bool test_face_align_negative_bbox() {
    cv::Mat img(480, 640, CV_8UC3, cv::Scalar(128, 128, 128));

    FaceDetection det;
    det.bbox = cv::Rect2f(-50.0f, -30.0f, 100.0f, 120.0f);

    FaceAlignOptions opt;
    opt.outW = 112;
    opt.outH = 112;

    auto r = alignFaceForArcFace112(img, det, opt);
    if (r.alignedBgr.empty()) { std::fprintf(stderr, "expected valid crop for partially negative bbox\n"); return false; }
    return true;
}

namespace {

// ArcFace 112 5 点模板的**副本**（`FaceAlign.cpp` 的 `arcFace112Ref5()` 位于匿名命名空间，未导出）。
// ⚠️ 同步义务**挂在下面这 5 个常量上**（不是"人脸方块"）：若模板被改而本副本未同步，src/dst 会
// 错配，用例**会红** —— 不是"结论不受影响"。复制而非导出模板，是刻意的取舍（导出只为测试更差）。
const std::array<cv::Point2f, 5>& ref112() {
    static const std::array<cv::Point2f, 5> kRef = {
        cv::Point2f(38.2946f, 51.6963f),
        cv::Point2f(73.5318f, 51.5014f),
        cv::Point2f(56.0252f, 71.7366f),
        cv::Point2f(41.5493f, 92.3655f),
        cv::Point2f(70.7299f, 92.2041f)};
    return kRef;
}

constexpr float kProbeScale = 2.0f;  // 源图中模板方形边长 = 112 × 2 = 224
constexpr float kProbeOff = 100.0f;

// 探针源图：黑底 + 亮色「人脸」方块。亮块取模板方形 [0,112]² 在源图中的**像** (100,100,224,224)
// —— 它代表"整张人脸"。于是「对齐后亮区是否铺满输出画布」成为可判定的量。
cv::Mat probeImage() {
    cv::Mat img(640, 640, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::rectangle(img,
                  cv::Rect(static_cast<int>(kProbeOff), static_cast<int>(kProbeOff),
                           static_cast<int>(112.0f * kProbeScale),
                           static_cast<int>(112.0f * kProbeScale)),
                  cv::Scalar(255, 255, 255), cv::FILLED);
    return img;
}

// 与上面亮块**精确相似**的 5 个关键点（模板点经 scale=2、offset=100）。
FaceDetection probeDetection() {
    FaceDetection det;
    det.bbox = cv::Rect2f(kProbeOff, kProbeOff, 112.0f * kProbeScale, 112.0f * kProbeScale);
    std::array<cv::Point2f, 5> kps{};
    for (size_t i = 0; i < ref112().size(); ++i) {
        kps[i] = cv::Point2f(ref112()[i].x * kProbeScale + kProbeOff,
                             ref112()[i].y * kProbeScale + kProbeOff);
    }
    det.keypoints5 = kps;
    return det;
}

// 断言「关键点路径对齐后，亮区铺满 outW×outH 画布」。
// 必须先钉死 `usedKeypoints5`：bbox 回退路径**也会**铺满画布，不钉就会「空转变绿」
//（本仓对这类测试静默通过有明确前科，故此断言不是可选项）。
bool assertKeypointsFillCanvas(int outW, int outH) {
    FaceAlignOptions opt;
    opt.outW = outW;
    opt.outH = outH;
    opt.preferKeypoints5 = true;

    const FaceAlignResult r = alignFaceForArcFace112(probeImage(), probeDetection(), opt);
    if (r.alignedBgr.empty()) {
        std::fprintf(stderr, "%dx%d: alignedBgr empty\n", outW, outH);
        return false;
    }
    if (!r.usedKeypoints5) {
        std::fprintf(stderr,
                     "%dx%d: expected keypoints path, got bbox fallback -> assertion would be vacuous\n",
                     outW, outH);
        return false;
    }
    if (r.alignedBgr.cols != outW || r.alignedBgr.rows != outH) {
        std::fprintf(stderr, "%dx%d: wrong output size (%dx%d)\n", outW, outH,
                     r.alignedBgr.cols, r.alignedBgr.rows);
        return false;
    }

    cv::Mat gray;
    cv::cvtColor(r.alignedBgr, gray, cv::COLOR_BGR2GRAY);
    cv::Mat mask;
    cv::threshold(gray, mask, 128, 255, cv::THRESH_BINARY);
    const cv::Rect bb = cv::boundingRect(mask);
    const double minRatio = 0.9;
    if (bb.width < outW * minRatio || bb.height < outH * minRatio) {
        std::fprintf(stderr,
                     "%dx%d: aligned face does not fill the canvas: content bbox=(%d,%d,%d,%d), "
                     "need >= %.0f%% of each axis -- the keypoints path is rendering at the fixed "
                     "112 reference scale instead of scaling the reference per axis to outW/outH\n",
                     outW, outH, bb.x, bb.y, bb.width, bb.height, minRatio * 100.0);
        return false;
    }
    return true;
}

}  // namespace

// ── 非方画布探针：用「模板坐标系里的小正方」而非"整张脸"，让三类错误实现都能被抓到 ─────────────
//
// 为什么不用"整张脸"（模板方形 [0,112]² 的像）作探针：它被画布裁剪后会**掩盖**错误实现。
// 实测证据：把实现改成"统一比例取大"（max）或"各轴独立"时，"整脸"版本的两条用例**都绿**（漏检）
// —— 因为此时亮块的像被 96 高的画布裁掉，其外接矩形与正确实现**恰好同为 (0,0,160,96)**。
// 改用模板坐标系里的居中正方 [28,84]²（边长 56）后，三类实现给出**互不相同**的像：
//   · 正确（等比 s=min(outW,outH)/112）⇒ 48×48（边长 = 56×96/112）  ← 期望
//   · 完全不缩放（原缺陷）          ⇒ 56×56
//   · 各轴独立 / 统一比例取大        ⇒ 实测 ~62×62 / 80×56（均被裁剪）
namespace {

cv::Mat probeInnerSquareImage() {
    const float a = 28.0f;   // 模板坐标系里居中正方的角（边长 = 84-28 = 56）
    const float b = 84.0f;
    cv::Mat img(640, 640, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::rectangle(img,
                  cv::Rect(static_cast<int>(kProbeOff + a * kProbeScale),
                           static_cast<int>(kProbeOff + a * kProbeScale),
                           static_cast<int>((b - a) * kProbeScale),
                           static_cast<int>((b - a) * kProbeScale)),
                  cv::Scalar(255, 255, 255), cv::FILLED);
    return img;
}

// 断言：模板坐标系里的正方（边长 56）经关键点路径对齐后，应仍是**正方**、边长 = 56 × s，
// 其中 s = min(outW,outH)/112（等比缩放到完整容纳画布）。
// 先钉死 `usedKeypoints5`：bbox 回退路径不会走到这里，不钉就可能「空转变绿」。
bool assertKeypointsUniformFit(int outW, int outH) {
    FaceAlignOptions opt;
    opt.outW = outW;
    opt.outH = outH;
    opt.preferKeypoints5 = true;

    const FaceAlignResult r = alignFaceForArcFace112(probeInnerSquareImage(), probeDetection(), opt);
    if (r.alignedBgr.empty()) {
        std::fprintf(stderr, "%dx%d: alignedBgr empty\n", outW, outH);
        return false;
    }
    if (!r.usedKeypoints5) {
        std::fprintf(stderr, "%dx%d: expected keypoints path, got bbox fallback -> vacuous\n", outW, outH);
        return false;
    }
    if (r.alignedBgr.cols != outW || r.alignedBgr.rows != outH) {
        std::fprintf(stderr, "%dx%d: wrong output size (%dx%d)\n", outW, outH,
                     r.alignedBgr.cols, r.alignedBgr.rows);
        return false;
    }

    cv::Mat gray;
    cv::cvtColor(r.alignedBgr, gray, cv::COLOR_BGR2GRAY);
    cv::Mat mask;
    cv::threshold(gray, mask, 128, 255, cv::THRESH_BINARY);
    const cv::Rect bb = cv::boundingRect(mask);
    if (bb.width <= 0 || bb.height <= 0) {
        std::fprintf(stderr, "%dx%d: empty bright region (no content rendered)\n", outW, outH);
        return false;
    }
    // 期望：模板正方（边长 56）经等比缩放 s = min(outW,outH)/112 后仍是正方，边长 = 56×s。
    const int side = std::min(outW, outH);
    const int want = static_cast<int>(std::lround(56.0 * static_cast<double>(side) / 112.0));
    const int tol = 2;   // 边缘插值允许 ±2px
    if (bb.width < want - tol || bb.width > want + tol || bb.height < want - tol ||
        bb.height > want + tol) {
        std::fprintf(stderr,
                     "%dx%d: template square (side 56) should map to a %dx%d square under uniform "
                     "scale s=min(outW,outH)/112, got bbox=%dx%d at (%d,%d) -- the reference points "
                     "are not being scaled uniformly to fit the canvas\n",
                     outW, outH, want, want, bb.width, bb.height, bb.x, bb.y);
        return false;
    }
    // 且不得溢出画布（等比缩放到"完整容纳"的语义）。
    if (bb.x < -tol || bb.y < -tol || bb.x + bb.width > outW + tol || bb.y + bb.height > outH + tol) {
        std::fprintf(stderr, "%dx%d: aligned content overflows the canvas (bbox=(%d,%d,%d,%d))\n",
                     outW, outH, bb.x, bb.y, bb.width, bb.height);
        return false;
    }
    return true;
}

}  // namespace

// 关键点仿射：**输出画布 ≠ 112 时，参考点必须按输出画布缩放**（s = min(outW,outH)/112，等比）。
//
// 被修的缺陷：`dst[i] = ref[i]` 直接取 112 模板坐标，随后却用 `warpAffine(..., Size(outW,outH))`
// 渲染 ⇒ 人脸按 **112 尺度**落在画布**左上角**：outW>112 时右/下留黑边、outW<112 时被裁掉。
// 而 bbox 回退路径是「裁剪后缩放**填满**画布」⇒ 方画布下两条路径本应一致，此前不一致。
//
// 判别力（已实测）：改回 `dst = ref` ⇒ 亮区落在 (0,0,112,112)，宽高 112 < 144 ⇒ **红**。
bool test_face_align_keypoints_out_non112() {
    return assertKeypointsFillCanvas(160, 160);
}

// 上一条只覆盖**方画布**（outW==outH）：此时 s=outW/112 恰好填满，测不出"非方画布该怎样"。
// 本用例用 **160×96** 钉死**非方画布**下的契约：参考几何**等比**缩放到**完整容纳**画布
// （s = min(outW,outH)/112），而不是各轴独立拉伸。
//
// ⚠️ 为什么是"等比容纳"而不是"各轴独立填满"：`estimateAffinePartial2D` 求的是**相似变换**
//（4 自由度：旋转 + 等比缩放 + 平移），**无法表达各轴独立缩放**；传入各轴不同的目标只会得到
// 最小二乘折中（实测：各轴独立时该正方被渲染成 76×75 一类的非正方）。故"与 bbox 回退路径
//（各轴独立的 cv::resize）在非方画布下完全对称"是**做不到**的——这是相似变换的能力边界。
//
// 判别力（三类错误实现**均已实测打红**，方画布那条仅第一类会红）：
//   · 完全不缩放（原缺陷）⇒ 56×56（应为 48×48）⇒ **红**；
//   · 各轴独立缩放       ⇒ 相似变换只给最小二乘折中（实测 ~62×62）⇒ **红**；
//   · 统一比例取大（max）⇒ 80×56（被裁剪）⇒ **红**。
bool test_face_align_keypoints_uniform_fit_non_square() {
    return assertKeypointsUniformFit(160, 96);
}
