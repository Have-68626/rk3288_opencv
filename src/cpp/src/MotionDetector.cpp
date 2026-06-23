#include "MotionDetector.h"
#include "Config.h"
#include <opencv2/imgproc.hpp>

MotionDetector::MotionDetector() {
}

bool MotionDetector::detect(const cv::Mat& currentFrame) {
    if (currentFrame.empty()) return false;

    cv::Mat gray, blurred;
    cv::cvtColor(currentFrame, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, blurred, cv::Size(21, 21), 0);

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (prevFrame.empty()) {
            cv::swap(blurred, prevFrame);
            return false;
        }

        cv::Mat diff;
        cv::absdiff(prevFrame, blurred, diff);
        cv::threshold(diff, motionMask, Config::MOTION_THRESHOLD, 255, cv::THRESH_BINARY);
        cv::swap(blurred, prevFrame);

        int changedPixels = cv::countNonZero(motionMask);
        return changedPixels > Config::MIN_MOTION_AREA;
    }
}

cv::Mat MotionDetector::getMotionMask() const {
    std::lock_guard<std::mutex> lock(mu_);
    return motionMask.clone();
}
