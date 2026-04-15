/**
 * @file VideoManager.cpp
 * @brief Implementation of VideoManager class.
 */
#include "VideoManager.h"
#include "Config.h"
#include "NativeLog.h"
#include <iostream>
#ifdef __linux__
#include <sys/resource.h>
#endif
#include <chrono>
#include <algorithm>
#include <vector>

VideoManager::VideoManager() : isRunning(false), hasNewFrame(false) {
}

VideoManager::~VideoManager() {
    close();
}

void VideoManager::setUseOpenCL(bool requested) {
    openCLRequested = requested;
    cv::ocl::setUseOpenCL(requested);
    bool effective = cv::ocl::useOpenCL();
    bool haveOpenCL = cv::ocl::haveOpenCL();
    std::string deviceName = "unknown";
    if (haveOpenCL) {
        try {
            cv::ocl::Device dev = cv::ocl::Device::getDefault();
            deviceName = dev.name();
        } catch (...) {
            // keep unknown
        }
    }

    std::string msg = "OpenCL process-wide requested=" + std::to_string(requested) +
                      " effective=" + std::to_string(effective) +
                      " haveOpenCL=" + std::to_string(haveOpenCL) +
                      " device=" + deviceName;
    rklog::logInfo("VideoManager", "setUseOpenCL", msg);
}

void VideoManager::setCancelToken(std::atomic<bool>* token) {
    cancelToken = token;
}

void VideoManager::setTimeoutsMs(int openTimeoutMs, int readTimeoutMs) {
    if (openTimeoutMs > 0) this->openTimeoutMs = openTimeoutMs;
    if (readTimeoutMs > 0) this->readTimeoutMs = readTimeoutMs;
}

bool VideoManager::open(int deviceId) {
    RKLOG_ENTER("VideoManager");
    if (isRunning) {
        return true; // Already running
    }

    cap.open(deviceId);
    if (!cap.isOpened()) {
        std::cerr << "Failed to open camera " << deviceId << std::endl;
        rklog::logError("VideoManager", __func__, "Failed to open camera");
        return false;
    }

    // Configure for performance
    cap.set(cv::CAP_PROP_FRAME_WIDTH, Config::FRAME_WIDTH);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, Config::FRAME_HEIGHT);
    cap.set(cv::CAP_PROP_FPS, Config::TARGET_FPS);

    rklog::logInfo("VideoManager", "open", "OpenCL requested=" + std::to_string(openCLRequested) + " effective=" + std::to_string(cv::ocl::useOpenCL()));

    // Start capture thread
    isRunning = true;
    captureThread = std::thread(&VideoManager::captureLoop, this);

    return true;
}

bool VideoManager::open(const std::string& filePath) {
    RKLOG_ENTER("VideoManager");
    if (isRunning) return true;

    if (cancelToken && cancelToken->load()) {
        return false;
    }

    isMockMode = true;
    isStaticImage = false;

    // Check extension (case-insensitive)
    std::string ext = "";
    size_t dotPos = filePath.find_last_of(".");
    if (dotPos != std::string::npos) {
        ext = filePath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }
    
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp" || ext == "webp") {
        isStaticImage = true;
        staticFrame = cv::imread(filePath);
        if (staticFrame.empty()) {
            std::cerr << "Failed to load image: " << filePath << std::endl;
            return false;
        }
        // Resize to match config if needed
        if (staticFrame.cols != Config::FRAME_WIDTH || staticFrame.rows != Config::FRAME_HEIGHT) {
            cv::resize(staticFrame, staticFrame, cv::Size(Config::FRAME_WIDTH, Config::FRAME_HEIGHT));
        }
    } else {
        auto tryOpen = [&](const std::string& pathOrUrl) -> bool {
            if (cancelToken && cancelToken->load()) {
                return false;
            }
            cv::VideoCapture tmp;
            std::vector<int> params;
            params.push_back(cv::CAP_PROP_OPEN_TIMEOUT_MSEC);
            params.push_back(openTimeoutMs);
            params.push_back(cv::CAP_PROP_READ_TIMEOUT_MSEC);
            params.push_back(readTimeoutMs);
            tmp.open(pathOrUrl, cv::CAP_ANY, params);
            if (!tmp.isOpened()) {
                return false;
            }
            cap = std::move(tmp);
            return true;
        };

        rklog::logInfo("VideoManager", "open", "OpenCL requested=" + std::to_string(openCLRequested) + " effective=" + std::to_string(cv::ocl::useOpenCL()));

        if (filePath.find("http") == 0 || filePath.find("rtsp") == 0 || filePath.find("rtmp") == 0) {
            std::string primary = filePath;
            std::string backup = "";
            size_t sep = filePath.find("|");
            if (sep != std::string::npos) {
                primary = filePath.substr(0, sep);
                backup = filePath.substr(sep + 1);
            }
            rklog::logInfo("VideoManager", "open", "Connecting to: " + primary);
            if (!tryOpen(primary)) {
                if (!backup.empty()) {
                    rklog::logInfo("VideoManager", "open", "Downgrading to backup: " + backup);
                    if (!tryOpen(backup)) {
                        return false;
                    }
                } else {
                    return false;
                }
            }
        } else {
            // Local file
            if (!tryOpen(filePath)) {
                std::cerr << "Failed to open video file: " << filePath << std::endl;
                return false;
            }
            cap.set(cv::CAP_PROP_FRAME_WIDTH, Config::FRAME_WIDTH);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, Config::FRAME_HEIGHT);
            cap.set(cv::CAP_PROP_FPS, Config::TARGET_FPS);
        }
    }
    
    if (cancelToken && cancelToken->load()) {
        if (cap.isOpened()) cap.release();
        return false;
    }

    isRunning = true;
    captureThread = std::thread(&VideoManager::captureLoop, this);
    return true;
}

void VideoManager::close() {
    RKLOG_ENTER("VideoManager");
    isRunning = false;
    if (captureThread.joinable()) {
        captureThread.join();
    }
    if (cap.isOpened()) {
        cap.release();
    }
}

bool VideoManager::getLatestFrame(cv::Mat& outFrame) {
    std::lock_guard<std::mutex> lock(frameMutex);
    if (!hasNewFrame || latestFrame.empty()) {
        return false;
    }
    latestFrame.copyTo(outFrame);
    // Optional: Reset hasNewFrame if we want to enforce consuming only new frames
    // hasNewFrame = false; 
    return true;
}

bool VideoManager::isOpened() const {
    return cap.isOpened();
}

void VideoManager::captureLoop() {
    // Set thread priority to high (Display/Audio priority) to ensure <500ms latency
#ifdef __linux__
    setpriority(PRIO_PROCESS, 0, -10);
#endif

    cv::Mat tempFrame;
    while (isRunning) {
        if (isMockMode && isStaticImage) {
            // Static Image Mode
            std::lock_guard<std::mutex> lock(frameMutex);
            staticFrame.copyTo(latestFrame);
            hasNewFrame = true;
            // Simulate FPS
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            continue;
        }

    if (cap.read(tempFrame)) {
        std::lock_guard<std::mutex> lock(frameMutex);
        if (!tempFrame.empty()) {
            // Ensure size
            if (tempFrame.cols != Config::FRAME_WIDTH || tempFrame.rows != Config::FRAME_HEIGHT) {
                cv::resize(tempFrame, tempFrame, cv::Size(Config::FRAME_WIDTH, Config::FRAME_HEIGHT));
            }
            // Swap buffers instead of copy if possible, or move
            // Since tempFrame is reused, we must copy. 
            // BUT: We can optimize by swapping if we had a pool. 
            // For now, let's keep copyTo but ensure latestFrame allocation is reused.
            tempFrame.copyTo(latestFrame); 
            hasNewFrame = true;
        }
    } else {
        // Handle EOF or Error
        if (isMockMode && cap.isOpened()) {
            // Rewind for video loop
            // Attempt to seek to frame 0
            cap.set(cv::CAP_PROP_POS_FRAMES, 0);
            continue;
        }
        
        // Handle read error or stream end
        // std::cerr << "Warning: Failed to read frame from camera." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    }
}
