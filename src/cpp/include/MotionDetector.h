#pragma once

#if __has_include(<opencv2/core.hpp>) && !defined(RK_SKIP_OPENCV)
#include <opencv2/core.hpp>
#else
namespace cv { class Mat; }
#endif
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
