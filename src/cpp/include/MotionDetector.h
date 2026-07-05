#pragma once

#include <opencv2/core.hpp>
#include <mutex>

namespace rk_core {

class MotionDetector {
public:
    MotionDetector();

    bool detect(const cv::Mat& currentFrame);

    /** Release internal frame buffers to free memory. Safe to call between stream changes. */
    void release();

    cv::Mat getMotionMask() const;

private:
    mutable std::mutex mu_;
    cv::Mat prevFrame;
    cv::Mat motionMask;
};

} // namespace rk_core
