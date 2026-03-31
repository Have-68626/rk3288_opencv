#pragma once

#include "FaceRecognizer.h"

#ifndef RK_WIN_HAS_OPENCV
#if __has_include(<opencv2/core.hpp>)
#define RK_WIN_HAS_OPENCV 1
#else
#define RK_WIN_HAS_OPENCV 0
#endif
#endif

#if RK_WIN_HAS_OPENCV
#include <opencv2/core.hpp>
#else
namespace cv {
struct Mat;
}
#endif

#include <vector>

namespace rk_win {

void drawFacesOverlay(cv::Mat& bgr, const std::vector<FaceMatch>& faces);

}  // namespace rk_win

