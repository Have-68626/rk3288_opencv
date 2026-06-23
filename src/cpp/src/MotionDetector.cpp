#include "MotionDetector.h"
#include "Config.h"
#include <opencv2/imgproc.hpp>

MotionDetector::MotionDetector() {
}

bool MotionDetector::detect(const cv::Mat& currentFrame) {
    if (currentFrame.empty()) return false;

    cv::Mat gray, blurred;
    if (currentFrame.channels() == 3) {
        cv::cvtColor(currentFrame, gray, cv::COLOR_BGR2GRAY);
    } else if (currentFrame.channels() == 4) {
        cv::cvtColor(currentFrame, gray, cv::COLOR_BGRA2GRAY);
    } else if (currentFrame.channels() == 1) {
        gray = currentFrame;
    } else {
        return false;
    }
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
        // MIN_MOTION_AREA = 500 是为 640x480 校准的绝对值;
        // 对于不同分辨率，按帧面积比例缩放
        const double areaScale = static_cast<double>(currentFrame.total()) / (640.0 * 480.0);
        const int minArea = static_cast<int>(Config::MIN_MOTION_AREA * areaScale);
        return changedPixels > minArea;
    }
}

void MotionDetector::release() {
    std::lock_guard<std::mutex> lock(mu_);
    prevFrame = cv::Mat();
    motionMask = cv::Mat();
}

cv::Mat MotionDetector::getMotionMask() const {
    std::lock_guard<std::mutex> lock(mu_);
    return motionMask.clone();
}
