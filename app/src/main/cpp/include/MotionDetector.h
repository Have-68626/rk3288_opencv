/**
 * @file MotionDetector.h
 * @brief Efficient motion detection algorithm.
 * 
 * Uses frame differencing to detect significant changes between consecutive frames.
 * Optimized for ARM Cortex-A17 by minimizing floating point operations.
 */
#pragma once

#include <opencv2/core.hpp>

class MotionDetector {
public:
    MotionDetector();
    
    /**
     * @brief Processes a frame to detect motion.
     * @param currentFrame The new video frame.
     * @return true if motion is detected above the configured threshold.
     */
    bool detect(const cv::Mat& currentFrame);

    /**
     * @brief Returns the mask of moving areas.
     */
    cv::Mat getMotionMask() const;

private:
    cv::Mat prevFrame;
    cv::Mat motionMask;
    cv::Mat grayFrame;
    cv::Mat blurredFrame;
};
