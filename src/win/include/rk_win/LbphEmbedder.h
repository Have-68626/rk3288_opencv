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
#else
namespace cv {
struct Mat;
}
#endif

#include <vector>

namespace rk_win {

class LbphEmbedder {
public:
    int gridX = 8;
    int gridY = 8;
    int resizeWidth = 128;
    int resizeHeight = 128;

    std::vector<float> embedFaceGray(const cv::Mat& faceGray) const;
    static double chiSquareDistance(const std::vector<float>& a, const std::vector<float>& b);
};

}  // namespace rk_win

