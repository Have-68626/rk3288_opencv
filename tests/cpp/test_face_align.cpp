#include <iostream>

#ifdef RK_SKIP_OPENCV
bool test_face_align_bbox_edge_cases() {
    std::cout << "SKIPPED: RK_SKIP_OPENCV is enabled" << std::endl;
    return true;
}
#else
#include "FaceAlign.h"
#include <opencv2/core.hpp>
#include <string>

bool test_face_align_bbox_edge_cases() {
    // Create a dummy image
    cv::Mat bgr(100, 100, CV_8UC3, cv::Scalar(0, 0, 0));

    FaceAlignOptions opt;
    opt.outW = 112;
    opt.outH = 112;
    opt.preferKeypoints5 = false; // Force bbox fallback

    // Test 1: Bbox entirely outside (negative coordinates)
    {
        FaceDetection det;
        det.bbox = cv::Rect2f(-50.0f, -50.0f, 20.0f, 20.0f);

        FaceAlignResult r = alignFaceForArcFace112(bgr, det, opt);
        if (r.err.empty() || r.err.find("FaceAlign: bbox 越界或为空") == std::string::npos) {
            return false;
        }
    }

    // Test 2: Bbox entirely outside (beyond image dimensions)
    {
        FaceDetection det;
        det.bbox = cv::Rect2f(150.0f, 150.0f, 20.0f, 20.0f);

        FaceAlignResult r = alignFaceForArcFace112(bgr, det, opt);
        if (r.err.empty() || r.err.find("FaceAlign: bbox 越界或为空") == std::string::npos) {
            return false;
        }
    }

    // Test 3: Bbox with zero area
    {
        FaceDetection det;
        det.bbox = cv::Rect2f(50.0f, 50.0f, 0.0f, 0.0f);

        FaceAlignResult r = alignFaceForArcFace112(bgr, det, opt);
        if (r.err.empty() || r.err.find("FaceAlign: bbox 越界或为空") == std::string::npos) {
            return false;
        }
    }

    // Test 4: Partially outside (should be clamped and succeed)
    {
        FaceDetection det;
        det.bbox = cv::Rect2f(80.0f, 80.0f, 40.0f, 40.0f);

        FaceAlignResult r = alignFaceForArcFace112(bgr, det, opt);
        if (!r.err.empty()) {
            return false;
        }
        if (!r.usedBboxFallback) {
            return false;
        }
        if (r.alignedBgr.cols != 112 || r.alignedBgr.rows != 112) {
            return false;
        }
    }
    return true;
}
#endif
