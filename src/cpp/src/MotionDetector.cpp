/**
 * @file MotionDetector.cpp
 * @brief Implementation of MotionDetector class.
 */
#include "MotionDetector.h"
#include "Config.h"
#include <opencv2/imgproc.hpp>

MotionDetector::MotionDetector() {
}

bool MotionDetector::detect(const cv::Mat& currentFrame) {
    if (currentFrame.empty()) return false;

    // Convert to grayscale for performance (1 channel vs 3)
    cv::cvtColor(currentFrame, grayFrame, cv::COLOR_BGR2GRAY);
    
    // Apply slight blur to reduce noise
    cv::GaussianBlur(grayFrame, blurredFrame, cv::Size(21, 21), 0);

    if (prevFrame.empty()) {
        blurredFrame.copyTo(prevFrame);
        return false;
    }

    // Compute absolute difference between current and previous frame
    cv::Mat diff;
    cv::absdiff(prevFrame, blurredFrame, diff);

    // Threshold the difference
    cv::threshold(diff, motionMask, Config::MOTION_THRESHOLD, 255, cv::THRESH_BINARY);

    // Update previous frame
    blurredFrame.copyTo(prevFrame);

    // Count non-zero pixels to determine if motion is significant
    int changedPixels = cv::countNonZero(motionMask);

    // Check against minimum area threshold
    return changedPixels > Config::MIN_MOTION_AREA;
}

cv::Mat MotionDetector::getMotionMask() const {
    return motionMask;
}
