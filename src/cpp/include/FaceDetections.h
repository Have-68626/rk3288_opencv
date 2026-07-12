#pragma once

#if __has_include(<opencv2/core.hpp>) && !defined(RK_SKIP_OPENCV)
#include <opencv2/core.hpp>
#else
namespace cv { class Mat; }
#endif

#include <array>
#include <optional>
#include <vector>

struct FaceDetection {
    cv::Rect2f bbox;
    float score = 0.0f;
    std::optional<std::array<cv::Point2f, 5>> keypoints5;
};

using FaceDetections = std::vector<FaceDetection>;

