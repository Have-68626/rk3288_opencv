#pragma once

#include <opencv2/core.hpp>
#include <mutex>

class MotionDetector {
public:
    MotionDetector();

    bool detect(const cv::Mat& currentFrame);

    cv::Mat getMotionMask() const;

private:
    mutable std::mutex mu_;
    cv::Mat prevFrame;
    cv::Mat motionMask;
};
