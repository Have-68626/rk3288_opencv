#pragma once

#ifndef RK_WIN_HAS_OPENCV
#if __has_include(<opencv2/core.hpp>)
#define RK_WIN_HAS_OPENCV 1
#else
#define RK_WIN_HAS_OPENCV 0
#endif
#endif

#if RK_WIN_HAS_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/objdetect.hpp>
#else
namespace cv {
struct Mat;
#ifndef RK_WIN_STUB_CV_RECT_DEFINED
#define RK_WIN_STUB_CV_RECT_DEFINED 1
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};
#endif
#ifndef RK_WIN_STUB_CV_CASCADE_CLASSIFIER_DEFINED
#define RK_WIN_STUB_CV_CASCADE_CLASSIFIER_DEFINED 1
class CascadeClassifier {};
#endif
}
#endif

#include <string>
#include <vector>

namespace rk_win {

class FaceDetector {
public:
    bool loadCascade(const std::string& cascadePath);
    std::vector<cv::Rect> detect(const cv::Mat& bgr, int minFaceSizePx);

private:
    cv::CascadeClassifier cascade_;
};

}  // namespace rk_win
