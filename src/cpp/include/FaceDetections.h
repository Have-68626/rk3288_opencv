#pragma once

#include <opencv2/core.hpp>

#include <array>
#include <optional>
#include <vector>

struct FaceDetection {
    cv::Rect2f bbox;
    float score = 0.0f;
    std::optional<std::array<cv::Point2f, 5>> keypoints5;
};

using FaceDetections = std::vector<FaceDetection>;

