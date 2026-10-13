#include "FaceAlign.h"
#include <opencv2/imgproc.hpp>
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
