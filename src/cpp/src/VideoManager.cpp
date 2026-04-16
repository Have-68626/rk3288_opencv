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
#include <fstream>

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
    std::string deviceVendor = "unknown";
    std::string deviceVersion = "unknown";
    std::string deviceType = "unknown";
    if (haveOpenCL) {
        try {
            cv::ocl::Device dev = cv::ocl::Device::getDefault();
            deviceName = dev.name();
            deviceVendor = dev.vendorName();
            deviceVersion = dev.version();
            int t = dev.type();
            if (t == cv::ocl::Device::TYPE_DEFAULT) deviceType = "DEFAULT";
            else if (t == cv::ocl::Device::TYPE_CPU) deviceType = "CPU";
            else if (t == cv::ocl::Device::TYPE_GPU) deviceType = "GPU";
            else if (t == cv::ocl::Device::TYPE_ACCELERATOR) deviceType = "ACCELERATOR";
            else deviceType = "OTHER";
        } catch (...) {
            // keep unknown
        }
    }

    std::string msg = "OpenCL process-wide requested=" + std::to_string(requested) +
                      " effective=" + std::to_string(effective) +
                      " haveOpenCL=" + std::to_string(haveOpenCL) +
                      " device=" + deviceName +
                      " vendor=" + deviceVendor +
                      " version=" + deviceVersion +
                      " type=" + deviceType;
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

    std::vector<int> params;
    params.push_back(cv::CAP_PROP_OPEN_TIMEOUT_MSEC);
    params.push_back(openTimeoutMs);
    params.push_back(cv::CAP_PROP_READ_TIMEOUT_MSEC);
    params.push_back(readTimeoutMs);
    try {
        cap.open(deviceId, cv::CAP_ANY, params);
    } catch (const std::exception& e) {
        std::cerr << "Exception in cap.open(device): " << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "Unknown exception in cap.open(device)" << std::endl;
        return false;
    }
    if (!cap.isOpened()) {
        std::cerr << "Failed to open camera " << deviceId << std::endl;
        rklog::logError("VideoManager", __func__, "Failed to open camera");
        return false;
    }

    // Configure for performance
    try {
        cap.set(cv::CAP_PROP_FRAME_WIDTH, Config::FRAME_WIDTH);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, Config::FRAME_HEIGHT);
        cap.set(cv::CAP_PROP_FPS, Config::TARGET_FPS);
    } catch (const std::exception& e) {
        std::cerr << "Exception in cap.set(device): " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Unknown exception in cap.set(device)" << std::endl;
    }

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
        try {
            // 内存监控与分块加载机制，防止超大文件导致 OOM 崩溃
            std::ifstream file(filePath, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                std::cerr << "Failed to open image file: " << filePath << std::endl;
                return false;
            }
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            
            // 内存监控：限制最大图片大小为 50MB (防止解码出几百MB或GB的矩阵)
            if (size > 50LL * 1024LL * 1024LL) { 
                std::cerr << "Image file too large (" << size << " bytes). Rejected to prevent OOM." << std::endl;
                return false;
            }

            // 分块加载 (Chunked reading)
            std::vector<char> buffer;
            buffer.reserve(static_cast<size_t>(size));
            const size_t chunkSize = 1024 * 1024; // 1MB chunks
            char chunk[1024 * 1024];
            while (file) {
                file.read(chunk, chunkSize);
                std::streamsize count = file.gcount();
                if (count > 0) {
                    buffer.insert(buffer.end(), chunk, chunk + count);
                }
            }
            
            cv::Mat rawData(1, buffer.size(), CV_8UC1, buffer.data());
            staticFrame = cv::imdecode(rawData, cv::IMREAD_COLOR);

            if (staticFrame.empty()) {
                std::cerr << "Failed to decode image: " << filePath << std::endl;
                return false;
            }
            // Resize to match config if needed
            if (staticFrame.cols != Config::FRAME_WIDTH || staticFrame.rows != Config::FRAME_HEIGHT) {
                cv::resize(staticFrame, staticFrame, cv::Size(Config::FRAME_WIDTH, Config::FRAME_HEIGHT));
            }
        } catch (const std::bad_alloc& e) {
            std::cerr << "OOM exception loading static image: " << e.what() << std::endl;
            return false;
        } catch (const cv::Exception& e) {
            std::cerr << "OpenCV exception loading static image: " << e.what() << std::endl;
            return false;
        } catch (const std::exception& e) {
            std::cerr << "Exception loading static image: " << e.what() << std::endl;
            return false;
        } catch (...) {
            std::cerr << "Unknown exception loading static image" << std::endl;
            return false;
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
            try {
                tmp.open(pathOrUrl, cv::CAP_ANY, params);
            } catch (const std::bad_alloc& e) {
                std::cerr << "OOM exception in tmp.open: " << e.what() << std::endl;
                return false;
            } catch (const cv::Exception& e) {
                std::cerr << "OpenCV Exception in tmp.open: " << e.what() << std::endl;
                return false;
            } catch (const std::exception& e) {
                std::cerr << "Exception in tmp.open: " << e.what() << std::endl;
                return false;
            } catch (...) {
                std::cerr << "Unknown exception in tmp.open" << std::endl;
                return false;
            }
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
            try {
                cap.set(cv::CAP_PROP_FRAME_WIDTH, Config::FRAME_WIDTH);
                cap.set(cv::CAP_PROP_FRAME_HEIGHT, Config::FRAME_HEIGHT);
                cap.set(cv::CAP_PROP_FPS, Config::TARGET_FPS);
            } catch (const std::exception& e) {
                std::cerr << "Exception in cap.set: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unknown exception in cap.set" << std::endl;
            }
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

        try {
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
        } catch (const std::exception& e) {
            std::cerr << "Exception in captureLoop: " << e.what() << std::endl;
            try {
                if (isMockMode && cap.isOpened()) {
                    cap.set(cv::CAP_PROP_POS_FRAMES, 0);
                }
            } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } catch (...) {
            std::cerr << "Unknown exception in captureLoop" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}
