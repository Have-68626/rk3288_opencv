/**
 * @file VideoManager.h
 * @brief Manages video capture device.
 * 
 * Handles camera initialization, frame grabbing, and resolution settings.
 * Implements a threaded capture mechanism to ensure low latency by always 
 * providing the most recent frame.
 */
#pragma once

#include <opencv2/opencv.hpp>
#include <opencv2/core/ocl.hpp>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>

class VideoManager {
public:
    VideoManager();
    ~VideoManager();

    void setCancelToken(std::atomic<bool>* token);
    void setTimeoutsMs(int openTimeoutMs, int readTimeoutMs);

    /**
     * @brief Set whether to use OpenCL globally.
     * @param requested true to request OpenCL enablement, false to disable.
     */
    void setUseOpenCL(bool requested);

    /**
     * @brief Opens the camera device.
     * @param deviceId Camera index (default 0).
     * @return true if successful.
     */
    bool open(int deviceId = 0);

    /**
     * @brief Opens a video file or image as a mock source.
     * @param filePath Path to file.
     * @return true if successful.
     */
    bool open(const std::string& filePath);

    /**
     * @brief Closes the camera device.
     */
    void close();

    /**
     * @brief Retrieves the latest captured frame.
     * @param outFrame Matrix to store the frame.
     * @return true if a valid frame is returned.
     */
    bool getLatestFrame(cv::Mat& outFrame);

    /**
     * @brief Checks if the camera is currently capturing.
     */
    bool isOpened() const;

private:
    void captureLoop();

    cv::VideoCapture cap;
    std::thread captureThread;
    std::atomic<bool> isRunning;
    std::atomic<bool>* cancelToken = nullptr;
    int openTimeoutMs = 5000;
    int readTimeoutMs = 5000;
    
    cv::Mat latestFrame;
    std::mutex frameMutex;
    bool hasNewFrame;
    
    // Mock Mode Support
    bool isMockMode = false;
    bool isStaticImage = false;
    cv::Mat staticFrame;
};
