#include "rk_win/FaceDetector.h"

#include <algorithm>
#include <opencv2/imgproc.hpp>

namespace rk_win {

bool FaceDetector::loadCascade(const std::string& cascadePath) {
    return cascade_.load(cascadePath);
}

std::vector<cv::Rect> FaceDetector::detect(const cv::Mat& bgr, int minFaceSizePx) {
    std::vector<cv::Rect> faces;
    if (bgr.empty()) return faces;
    if (cascade_.empty()) return faces;

    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::equalizeHist(gray, gray);

    cascade_.detectMultiScale(gray, faces, 1.1, 6, 0, cv::Size(minFaceSizePx, minFaceSizePx));
    std::sort(faces.begin(), faces.end(), [](const cv::Rect& a, const cv::Rect& b) { return a.area() > b.area(); });
    return faces;
}

}  // namespace rk_win

