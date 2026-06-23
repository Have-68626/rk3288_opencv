/**
 * @file VideoManager.cpp
 * @brief Implementation of VideoManager class.
 */
#include "VideoManager.h"
#include "Config.h"
#include "MppDecoder.h"
#include "NativeLog.h"
#include <iostream>
#ifdef __linux__
#include <sys/resource.h>
#endif
#include <array>
#include <chrono>
#include <algorithm>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cstdint>

namespace {

constexpr std::uintmax_t kMockMaxImageBytes = 50ULL * 1024ULL * 1024ULL;

bool isStaticImageExt(const std::string& ext) {
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp" || ext == "webp";
}

bool hasValidImageMagic(const std::string& ext, const std::array<unsigned char, 16>& magic, size_t len) {
    if ((ext == "jpg" || ext == "jpeg") && len >= 3) {
        return magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF;
    }
    if (ext == "png" && len >= 8) {
        return magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47 &&
               magic[4] == 0x0D && magic[5] == 0x0A && magic[6] == 0x1A && magic[7] == 0x0A;
    }
    if (ext == "bmp" && len >= 2) {
        return magic[0] == 0x42 && magic[1] == 0x4D;
    }
    if (ext == "webp" && len >= 12) {
        return magic[0] == 0x52 && magic[1] == 0x49 && magic[2] == 0x46 && magic[3] == 0x46 &&
               magic[8] == 0x57 && magic[9] == 0x45 && magic[10] == 0x42 && magic[11] == 0x50;
    }
    return false;
}

}  // namespace

static const char* mockStateName(VideoManager::MockState s) {
    switch (s) {
        case VideoManager::MockState::NONE:         return "NONE";
        case VideoManager::MockState::INIT:          return "INIT";
        case VideoManager::MockState::PREFLIGHT_OK:  return "PREFLIGHT_OK";
        case VideoManager::MockState::LOADING:       return "LOADING";
        case VideoManager::MockState::RUNNING:       return "RUNNING";
        case VideoManager::MockState::FAILED:        return "FAILED";
        case VideoManager::MockState::FALLBACK:      return "FALLBACK";
    }
    return "UNKNOWN";
}

VideoManager::VideoManager() : isRunning(false), hasNewFrame(false) {
}

VideoManager::~VideoManager() {
    close();
}

bool VideoManager::isUrlSource(const std::string& path) {
    return path.rfind("rtsp://", 0) == 0 || path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0 || path.rfind("rtmp://", 0) == 0;
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

void VideoManager::setMockRejectReason(const std::string& reasonCode, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lock(mockMetaMutex_);
        lastMockRejectReason_ = reasonCode;
    }
    (void)detail;
}

void VideoManager::clearMockRejectReason() {
    std::lock_guard<std::mutex> lock(mockMetaMutex_);
    lastMockRejectReason_.clear();
}

bool VideoManager::preflightMockInput(const std::string& filePath, std::string* reasonCode) {
    auto setReason = [&](const std::string& code) {
        if (reasonCode) *reasonCode = code;
        return false;
    };

    if (filePath.empty()) {
        return setReason("MOCK_IO_OPEN_FAILED");
    }

    std::string ext;
    const size_t dotPos = filePath.find_last_of(".");
    if (dotPos != std::string::npos) {
        ext = filePath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    if (isUrlSource(filePath)) {
        return true;
    }

    std::error_code fsec;
    const bool exists = std::filesystem::exists(filePath, fsec);
    if (fsec || !exists) {
        return setReason("MOCK_IO_OPEN_FAILED");
    }

    const auto fileSize = std::filesystem::file_size(filePath, fsec);
    if (fsec) {
        return setReason("MOCK_IO_OPEN_FAILED");
    }

    if (fileSize < 3) {
        return setReason("MOCK_FILE_INCOMPLETE");
    }

    if (!isStaticImageExt(ext)) {
        return true;
    }

    if (fileSize > kMockMaxImageBytes) {
        return setReason("MOCK_OVERSIZE");
    }

    std::ifstream magicFile(filePath, std::ios::binary);
    if (!magicFile.is_open()) {
        return setReason("MOCK_IO_OPEN_FAILED");
    }
    std::array<unsigned char, 16> magic{};
    magicFile.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    const size_t magicLen = static_cast<size_t>(std::max<std::streamsize>(0, magicFile.gcount()));
    if (!hasValidImageMagic(ext, magic, magicLen)) {
        if (magicLen < 3) {
            return setReason("MOCK_FILE_INCOMPLETE");
        }
        return setReason("MOCK_MAGIC_INVALID");
    }

    return true;
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

    clearMockRejectReason();
    isMockMode = true;
    isStaticImage = false;
    mockState = MockState::INIT;
    mockFilePath = filePath;
    rklog::logInfo("MockMode", "open", "Mock file path=" + filePath);

    // Check extension (case-insensitive)
    std::string ext = "";
    size_t dotPos = filePath.find_last_of(".");
    if (dotPos != std::string::npos) {
        ext = filePath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    {
        std::string reason;
        if (!preflightMockInput(filePath, &reason)) {
            mockState = MockState::FAILED;
            setMockRejectReason(reason, "calling_phase_reject");
            return false;
        }
    }

    if (isStaticImageExt(ext)) {
        isStaticImage = true;
        mockState = MockState::PREFLIGHT_OK;
        rklog::logInfo("MockMode", "open", "Static image, state=PREFLIGHT_OK ext=" + ext);
        try {
            // 内存监控与分块加载机制，防止超大文件导致 OOM 崩溃
            std::ifstream file(filePath, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                std::cerr << "Failed to open image file: " << filePath << std::endl;
                mockState = MockState::FAILED;
                setMockRejectReason("MOCK_IO_OPEN_FAILED", "image_open_failed");
                return false;
            }
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            
            // 内存监控：限制最大图片大小为 50MB (防止解码出几百MB或GB的矩阵)
            if (size > 50LL * 1024LL * 1024LL) { 
                std::cerr << "Image file too large (" << size << " bytes). Rejected to prevent OOM." << std::endl;
                mockState = MockState::FAILED;
                setMockRejectReason("MOCK_OVERSIZE", "bytes=" + std::to_string(static_cast<unsigned long long>(size)));
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
                mockState = MockState::FAILED;
                setMockRejectReason("MOCK_DECODE_FAILED", "static_image_decode_failed");
                rklog::logError("MockMode", "open", "Failed to decode static image");
                return false;
            }
            // Resize to match config if needed
            if (staticFrame.cols != Config::FRAME_WIDTH || staticFrame.rows != Config::FRAME_HEIGHT) {
                cv::resize(staticFrame, staticFrame, cv::Size(Config::FRAME_WIDTH, Config::FRAME_HEIGHT));
            }
            mockState = MockState::LOADING;
            rklog::logInfo("MockMode", "open", "Static image decoded, w=" + std::to_string(staticFrame.cols) +
                " h=" + std::to_string(staticFrame.rows) + " state=LOADING");
        } catch (const std::bad_alloc& e) {
            std::cerr << "OOM exception loading static image: " << e.what() << std::endl;
            mockState = MockState::FAILED;
            setMockRejectReason("MOCK_OOM", e.what());
            rklog::logError("MockMode", "open", "OOM loading static image: " + std::string(e.what()));
            return false;
        } catch (const cv::Exception& e) {
            std::cerr << "OpenCV exception loading static image: " << e.what() << std::endl;
            mockState = MockState::FAILED;
            setMockRejectReason("MOCK_DECODE_FAILED", e.what());
            rklog::logError("MockMode", "open", "OpenCV exception: " + std::string(e.what()));
            return false;
        } catch (const std::exception& e) {
            std::cerr << "Exception loading static image: " << e.what() << std::endl;
            mockState = MockState::FAILED;
            setMockRejectReason("MOCK_UNKNOWN_ERROR", e.what());
            rklog::logError("MockMode", "open", "Exception: " + std::string(e.what()));
            return false;
        } catch (...) {
            std::cerr << "Unknown exception loading static image" << std::endl;
            mockState = MockState::FAILED;
            setMockRejectReason("MOCK_UNKNOWN_ERROR", "unknown_exception");
            rklog::logError("MockMode", "open", "Unknown exception");
            return false;
        }
    } else {
        // Video file or network stream
        mockState = MockState::PREFLIGHT_OK;
        rklog::logInfo("MockMode", "open", "Video/stream source, ext=" + ext + " state=PREFLIGHT_OK");

        auto tryOpen = [&](const std::string& pathOrUrl) -> bool {
            if (cancelToken && cancelToken->load()) {
                return false;
            }
            cv::VideoCapture tmp;
            std::vector<int> params;
            // Use mock-specific timeout for network streams
            int effectiveTimeout = (pathOrUrl.find("http") == 0 || pathOrUrl.find("rtsp") == 0 || pathOrUrl.find("rtmp") == 0)
                ? static_cast<int>(mockLoadTimeoutMs) : openTimeoutMs;
            params.push_back(cv::CAP_PROP_OPEN_TIMEOUT_MSEC);
            params.push_back(effectiveTimeout);
            params.push_back(cv::CAP_PROP_READ_TIMEOUT_MSEC);
            params.push_back(readTimeoutMs);
            rklog::logInfo("MockMode", "tryOpen", "Opening: " + pathOrUrl + " timeoutMs=" + std::to_string(effectiveTimeout));
            try {
                tmp.open(pathOrUrl, cv::CAP_ANY, params);
            } catch (const std::bad_alloc& e) {
                rklog::logError("MockMode", "tryOpen", "OOM: " + std::string(e.what()));
                return false;
            } catch (const cv::Exception& e) {
                rklog::logError("MockMode", "tryOpen", "OpenCV Exception: " + std::string(e.what()));
                return false;
            } catch (const std::exception& e) {
                rklog::logError("MockMode", "tryOpen", "Exception: " + std::string(e.what()));
                return false;
            } catch (...) {
                rklog::logError("MockMode", "tryOpen", "Unknown exception");
                return false;
            }
            if (!tmp.isOpened()) {
                rklog::logError("MockMode", "tryOpen", "Failed to open (timeout or unsupported format)");
                setMockRejectReason("MOCK_OPEN_FAILED", "try_open_failed");
                return false;
            }
            rklog::logInfo("MockMode", "tryOpen", "Opened successfully");
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
            mockState = MockState::LOADING;
            if (!tryOpen(primary)) {
                rklog::logError("MockMode", "open", "Primary stream failed, state=FAILED");
                if (!backup.empty()) {
                    rklog::logInfo("MockMode", "open", "Trying backup: " + backup);
                    if (!tryOpen(backup)) {
                        mockState = MockState::FAILED;
                        setMockRejectReason("MOCK_OPEN_FAILED", "stream_primary_backup_failed");
                        rklog::logError("MockMode", "open", "Backup also failed, state=FAILED");
                        return false;
                    }
                } else {
                    mockState = MockState::FAILED;
                    setMockRejectReason("MOCK_OPEN_FAILED", "stream_primary_failed_no_backup");
                    rklog::logError("MockMode", "open", "No backup, state=FAILED");
                    return false;
                }
            }
        } else {
            // Local file — try MPP hardware decoding first
            mockState = MockState::LOADING;
            rklog::logInfo("MockMode", "open", "Opening local file, state=LOADING");
#if defined(RK_HAVE_MPP) && RK_HAVE_MPP
            mppDecoder = std::make_unique<MppDecoder>();
            if (mppDecoder->init() && mppDecoder->open(filePath)) {
                useMppDecode = true;
                rklog::logInfo("MockMode", "open", "MPP hardware decoding enabled for: " + filePath);
                // Also initialize cap as fallback in case MPP fails at runtime
                if (!tryOpen(filePath)) {
                    rklog::logWarn("MockMode", "open", "MPP backup cap.open failed (non-fatal, MPP will be primary)");
                } else {
                    try {
                        cap.set(cv::CAP_PROP_FRAME_WIDTH, Config::FRAME_WIDTH);
                        cap.set(cv::CAP_PROP_FRAME_HEIGHT, Config::FRAME_HEIGHT);
                        cap.set(cv::CAP_PROP_FPS, Config::TARGET_FPS);
                    } catch (...) {}
                }
            } else {
                useMppDecode = false;
                mppDecoder.reset();
                rklog::logInfo("MockMode", "open", "MPP unavailable, falling back to OpenCV VideoCapture");
                if (!tryOpen(filePath)) {
                    mockState = MockState::FAILED;
                    setMockRejectReason("MOCK_OPEN_FAILED", "local_video_open_failed");
                    rklog::logError("MockMode", "open", "Failed to open local video file, state=FAILED");
                    return false;
                }
                try {
                    cap.set(cv::CAP_PROP_FRAME_WIDTH, Config::FRAME_WIDTH);
                    cap.set(cv::CAP_PROP_FRAME_HEIGHT, Config::FRAME_HEIGHT);
                    cap.set(cv::CAP_PROP_FPS, Config::TARGET_FPS);
                } catch (const std::exception& e) {
                    rklog::logWarn("MockMode", "open", "cap.set exception: " + std::string(e.what()));
                } catch (...) {
                    rklog::logWarn("MockMode", "open", "cap.set unknown exception");
                }
            }
#else
            useMppDecode = false;
            mppDecoder.reset();
            rklog::logInfo("MockMode", "open", "MPP disabled for this build, using OpenCV VideoCapture");
            if (!tryOpen(filePath)) {
                mockState = MockState::FAILED;
                setMockRejectReason("MOCK_OPEN_FAILED", "local_video_open_failed");
                rklog::logError("MockMode", "open", "Failed to open local video file, state=FAILED");
                return false;
            }
            try {
                cap.set(cv::CAP_PROP_FRAME_WIDTH, Config::FRAME_WIDTH);
                cap.set(cv::CAP_PROP_FRAME_HEIGHT, Config::FRAME_HEIGHT);
                cap.set(cv::CAP_PROP_FPS, Config::TARGET_FPS);
            } catch (const std::exception& e) {
                rklog::logWarn("MockMode", "open", "cap.set exception: " + std::string(e.what()));
            } catch (...) {
                rklog::logWarn("MockMode", "open", "cap.set unknown exception");
            }
#endif
        }
    }
    
    if (cancelToken && cancelToken->load()) {
        if (cap.isOpened()) cap.release();
        return false;
    }

    mockState = MockState::LOADING;
    isRunning = true;
    captureThread = std::thread(&VideoManager::captureLoop, this);
    rklog::logInfo("MockMode", "open", "Capture thread started, state=LOADING");
    return true;
}

void VideoManager::close() {
    RKLOG_ENTER("VideoManager");
    if (isMockMode) {
        rklog::logInfo("MockMode", "close", "Closing mock source, current state=" + std::string(mockStateName(mockState)));
    }
    isRunning = false;
    if (captureThread.joinable()) {
        captureThread.join();
    }
    if (cap.isOpened()) {
        cap.release();
    }
    if (isMockMode) {
        mockState = MockState::NONE;
        mockFilePath.clear();
        clearMockRejectReason();
        if (mppDecoder) {
            mppDecoder->close();
            mppDecoder.reset();
        }
        useMppDecode = false;
        isMockMode = false;
        isStaticImage = false;
        rklog::logInfo("MockMode", "close", "Mock source closed, state=NONE");
    }
}

bool VideoManager::getLatestFrame(cv::Mat& outFrame) {
    std::lock_guard<std::mutex> lock(frameMutex);
    if (!hasNewFrame || latestFrame.empty()) {
        return false;
    }
    latestFrame.copyTo(outFrame);
    return true;
}

bool VideoManager::isOpened() const {
    return cap.isOpened();
}

VideoManager::MockState VideoManager::getMockState() const {
    return mockState;
}

std::string VideoManager::getMockFilePath() const {
    return mockFilePath;
}

std::string VideoManager::getLastMockRejectReason() const {
    std::lock_guard<std::mutex> lock(mockMetaMutex_);
    return lastMockRejectReason_;
}

void VideoManager::captureLoop() {
    // Set thread priority to high (Display/Audio priority) to ensure <500ms latency
#ifdef __linux__
    setpriority(PRIO_PROCESS, 0, -10);
#endif

    cv::Mat tempFrame;
    bool firstFrameValidated = false;
    while (isRunning) {
        // MPP hardware decoding branch (mock mode video files)
        if (isMockMode && useMppDecode && mppDecoder) {
            if (mppDecoder->read(tempFrame)) {
                std::lock_guard<std::mutex> lock(frameMutex);
                if (!tempFrame.empty()) {
                    if (!firstFrameValidated && isMockMode) {
                        int ch = tempFrame.channels();
                        bool valid = tempFrame.cols > 0 && tempFrame.cols <= 4096
                            && tempFrame.rows > 0 && tempFrame.rows <= 4096
                            && (ch == 3 || ch == 4);
                        if (valid) {
                            mockState = MockState::RUNNING;
                            rklog::logInfo("MockMode", "captureLoop", "MPP first frame valid, w=" +
                                std::to_string(tempFrame.cols) + " h=" + std::to_string(tempFrame.rows) +
                                " ch=" + std::to_string(ch) + " state=RUNNING");
                        } else {
                            mockState = MockState::FAILED;
                            setMockRejectReason("MOCK_FRAME_INVALID", "mpp_first_frame_invalid");
                            rklog::logError("MockMode", "captureLoop", "MPP first frame invalid, w=" +
                                std::to_string(tempFrame.cols) + " h=" + std::to_string(tempFrame.rows) +
                                " ch=" + std::to_string(ch) + " state=FAILED");
                        }
                        firstFrameValidated = true;
                    }
                    tempFrame.copyTo(latestFrame);
                    hasNewFrame = true;
                }
            } else if (!mppDecoder->isOpened()) {
                // MPP decoding failed or EOS, try to re-init
                rklog::logWarn("MockMode", "captureLoop", "MPP decode failed, falling back to VideoCapture");
                useMppDecode = false;
                mppDecoder.reset();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            continue;
        }

        if (isMockMode && isStaticImage) {
            // Static Image Mode: verified at open() time
            if (!firstFrameValidated) {
                int ch = staticFrame.channels();
                bool valid = !staticFrame.empty()
                    && staticFrame.cols > 0 && staticFrame.cols <= 4096
                    && staticFrame.rows > 0 && staticFrame.rows <= 4096
                    && (ch == 3 || ch == 4);
                if (valid) {
                    mockState = MockState::RUNNING;
                    rklog::logInfo("MockMode", "captureLoop", "Static image valid, w=" +
                        std::to_string(staticFrame.cols) + " h=" + std::to_string(staticFrame.rows) +
                        " ch=" + std::to_string(ch) + " state=RUNNING");
                } else {
                    mockState = MockState::FAILED;
                    setMockRejectReason("MOCK_FRAME_INVALID", "static_frame_invalid");
                    rklog::logError("MockMode", "captureLoop", "Static image invalid, w=" +
                        std::to_string(staticFrame.cols) + " h=" + std::to_string(staticFrame.rows) +
                        " ch=" + std::to_string(ch) + " state=FAILED");
                }
                firstFrameValidated = true;
            }
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
                    // First frame validation for mock mode video sources
                    if (!firstFrameValidated && isMockMode) {
                        int ch = tempFrame.channels();
                        bool valid = tempFrame.cols > 0 && tempFrame.cols <= 4096
                            && tempFrame.rows > 0 && tempFrame.rows <= 4096
                            && (ch == 3 || ch == 4);
                        if (valid) {
                            mockState = MockState::RUNNING;
                            rklog::logInfo("MockMode", "captureLoop", "First frame valid, w=" +
                                std::to_string(tempFrame.cols) + " h=" + std::to_string(tempFrame.rows) +
                                " ch=" + std::to_string(ch) + " state=RUNNING");
                        } else {
                            mockState = MockState::FAILED;
                            setMockRejectReason("MOCK_FRAME_INVALID", "video_first_frame_invalid");
                            rklog::logError("MockMode", "captureLoop", "First frame invalid, w=" +
                                std::to_string(tempFrame.cols) + " h=" + std::to_string(tempFrame.rows) +
                                " ch=" + std::to_string(ch) + " state=FAILED");
                        }
                        firstFrameValidated = true;
                    }
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
