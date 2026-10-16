/**
 * @file VideoManager.cpp
 * @brief Implementation of VideoManager class.
 */
#include "VideoManager.h"
#include "Config.h"
#include "MppDecoder.h"
#include "AccelerationContract.h"
#include "NativeLog.h"
#include <iostream>
#ifdef __linux__
#include <sys/resource.h>
#endif
#include <chrono>
#include <algorithm>
#include <cstring>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cstdint>

using namespace rk_core;

namespace {

constexpr std::uintmax_t kMockMaxImageBytes = 50ULL * 1024ULL * 1024ULL;

// 解码前尺寸边界的产品上界：与 captureLoop 中帧校验一致（cols/rows <= 4096）。
constexpr std::int64_t kMockMaxImageDimension = 4096;
// 头部扫描窗口：JPEG 的 SOF 标记需跨过 APPn/EXIF 段，取 64KB 覆盖常见头部。
constexpr std::size_t kMockHeaderScanBytes = 65536;

struct DeclaredDims {
    std::int64_t width = 0;
    std::int64_t height = 0;
    bool parsed = false;
};

// 从文件头解析声明像素尺寸，供解码前预检拒绝超界样本，
// 防止 imdecode 依据伪造头部分配巨大矩阵导致 OOM。解析失败放行（fail-open）。
static DeclaredDims parsePngDims(const unsigned char* d, std::size_t len) {
    // 签名(8) + 长度(4) + "IHDR"(4) + 宽(4 BE) + 高(4 BE)
    if (len < 24 || std::memcmp(d + 12, "IHDR", 4) != 0) return {};
    const std::uint32_t w = (static_cast<std::uint32_t>(d[16]) << 24) | (static_cast<std::uint32_t>(d[17]) << 16) |
                            (static_cast<std::uint32_t>(d[18]) << 8) | d[19];
    const std::uint32_t h = (static_cast<std::uint32_t>(d[20]) << 24) | (static_cast<std::uint32_t>(d[21]) << 16) |
                            (static_cast<std::uint32_t>(d[22]) << 8) | d[23];
    return { static_cast<std::int64_t>(w), static_cast<std::int64_t>(h), true };
}

static DeclaredDims parseBmpDims(const unsigned char* d, std::size_t len) {
    // "BM"(0-1) + 信息头。biSize(14-17 LE)：40=标准 32 位宽/高(18-25)；12=OS/2 core-header，
    // 宽/高为 16 位字段(18-21)。不识别 biSize 时 fail-open（由解码后帧校验兜底），
    // 避免把合法 OS/2 BMP 的 planes|bpp 误读为巨大宽/高而误拒。
    if (len < 26 || d[0] != 'B' || d[1] != 'M') return {};
    const std::uint32_t biSize = static_cast<std::uint32_t>(d[14]) | (static_cast<std::uint32_t>(d[15]) << 8) |
                                 (static_cast<std::uint32_t>(d[16]) << 16) | (static_cast<std::uint32_t>(d[17]) << 24);
    if (biSize == 12) {
        // OS/2 core-header：16 位无符号宽/高（上限 65535，仍受 4096 上界防护）
        const std::int64_t w = static_cast<std::uint32_t>(d[18]) | (static_cast<std::uint32_t>(d[19]) << 8);
        const std::int64_t h = static_cast<std::uint32_t>(d[20]) | (static_cast<std::uint32_t>(d[21]) << 8);
        return { w, h, true };
    }
    if (biSize < 40) return {};  // 不识别 header 变体，fail-open
    const std::uint32_t rawW = static_cast<std::uint32_t>(d[18]) | (static_cast<std::uint32_t>(d[19]) << 8) |
                               (static_cast<std::uint32_t>(d[20]) << 16) | (static_cast<std::uint32_t>(d[21]) << 24);
    const std::uint32_t rawH = static_cast<std::uint32_t>(d[22]) | (static_cast<std::uint32_t>(d[23]) << 8) |
                               (static_cast<std::uint32_t>(d[24]) << 16) | (static_cast<std::uint32_t>(d[25]) << 24);
    std::int64_t w = static_cast<std::int64_t>(static_cast<std::int32_t>(rawW));
    std::int64_t h = static_cast<std::int64_t>(static_cast<std::int32_t>(rawH));
    if (w < 0) w = -w;
    if (h < 0) h = -h;
    return { w, h, true };
}

static DeclaredDims parseJpegDims(const unsigned char* d, std::size_t len) {
    // SOI(0-1) 后扫描标记段，命中 SOF0-15 读取 高(2 BE)+宽(2 BE)
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return {};
    std::size_t i = 2;
    while (i + 4 <= len) {
        if (d[i] != 0xFF) { i++; continue; }
        const std::uint8_t m = d[i + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }  // SOI/TEM/RSTn
        if (m == 0xD9) break;  // EOI
        const int segLen = (d[i + 2] << 8) | d[i + 3];
        if (segLen < 2) break;
        const bool isSof = m == 0xC0 || m == 0xC1 || m == 0xC2 || m == 0xC3 || m == 0xC5 ||
                           m == 0xC6 || m == 0xC7 || m == 0xC9 || m == 0xCA || m == 0xCB ||
                           m == 0xCD || m == 0xCE || m == 0xCF;
        if (isSof) {
            if (i + 9 <= len) {
                const int h = (d[i + 5] << 8) | d[i + 6];
                const int w = (d[i + 7] << 8) | d[i + 8];
                return { w, h, true };
            }
            break;
        }
        i += 2 + static_cast<std::size_t>(segLen);
    }
    return {};
}

static DeclaredDims parseWebpDims(const unsigned char* d, std::size_t len) {
    if (len < 30 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WEBP", 4) != 0) return {};
    const std::string chunk(reinterpret_cast<const char*>(d + 12), 4);
    if (chunk == "VP8X") {
        // 扩展头：画布 宽-1/高-1 为 24 位 LE（24-26 宽-1，27-29 高-1）
        const std::uint32_t w = static_cast<std::uint32_t>(d[24]) | (static_cast<std::uint32_t>(d[25]) << 8) |
                                (static_cast<std::uint32_t>(d[26]) << 16);
        const std::uint32_t h = static_cast<std::uint32_t>(d[27]) | (static_cast<std::uint32_t>(d[28]) << 8) |
                                (static_cast<std::uint32_t>(d[29]) << 16);
        return { static_cast<std::int64_t>(w) + 1, static_cast<std::int64_t>(h) + 1, true };
    }
    if (chunk == "VP8 ") {
        // 关键帧：帧标签(20-22) + 起始码 0x9D 01 2A(23-25)；宽 14 位 LE(26-27)、高 14 位 LE(28-29)
        if (d[23] != 0x9D || d[24] != 0x01 || d[25] != 0x2A) return {};
        const std::uint32_t w = (static_cast<std::uint32_t>(d[26]) | (static_cast<std::uint32_t>(d[27]) << 8)) & 0x3FFF;
        const std::uint32_t h = (static_cast<std::uint32_t>(d[28]) | (static_cast<std::uint32_t>(d[29]) << 8)) & 0x3FFF;
        return { static_cast<std::int64_t>(w), static_cast<std::int64_t>(h), true };
    }
    if (chunk == "VP8L") {
        // 无损：签名 0x2F(20)；宽-1/高-1 各 14 位 LSB-first(21-24)
        if (d[20] != 0x2F) return {};
        const std::uint32_t wm1 = static_cast<std::uint32_t>(d[21]) |
                                  ((static_cast<std::uint32_t>(d[22]) & 0x3F) << 8);
        const std::uint32_t hm1 = (static_cast<std::uint32_t>(d[22]) >> 6) |
                                  (static_cast<std::uint32_t>(d[23]) << 2) |
                                  ((static_cast<std::uint32_t>(d[24]) & 0x0F) << 10);
        return { static_cast<std::int64_t>(wm1) + 1, static_cast<std::int64_t>(hm1) + 1, true };
    }
    return {};
}

bool isStaticImageExt(const std::string& ext) {
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp" || ext == "webp";
}

bool hasValidImageMagic(const std::string& ext, const unsigned char* magic, size_t len) {
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

std::string VideoManager::sanitizeUriForLog(const std::string& url) {
    // S-SEC-B07-002：URI 日志脱敏。URI userinfo（scheme://user:pass@host）属敏感凭据，
    // operator 输入的 RTSP/HTTP/RTMP 流地址若含 userinfo，原样写入日志即端到端凭据泄漏
    // （VideoManager→NativeLog 文件/Logcat）。这里仅在日志层掩码：保留 scheme://host:port
    // 可读性，将整个 userinfo 段替换为 ***（不暴露 user 也不暴露 pass）；非 URL 输入
    // （无 ://，如本地文件路径）原样返回，不影响网络行为。
    const size_t schemePos = url.find("://");
    if (schemePos == std::string::npos) return url;  // 非 URL（本地路径）原样返回

    // authority 段：scheme:// 之后、第一个路径/查询/片段分隔符（/ ? #）之前
    const size_t authorityStart = schemePos + 3;
    const size_t pathSep = url.find_first_of("/?#", authorityStart);
    const size_t authorityEnd = (pathSep == std::string::npos) ? url.size() : pathSep;
    if (authorityEnd <= authorityStart) return url;  // 空 authority（如 file:///）原样返回

    // 在 authority 内找最后一个 '@'（userinfo 与 host 的分隔符）；@ 出现在路径/查询时不计
    size_t at = std::string::npos;
    for (size_t i = authorityStart; i < authorityEnd; ++i) {
        if (url[i] == '@') at = i;
    }
    if (at == std::string::npos) return url;  // 无 userinfo 原样返回

    // 将 userinfo 段（authorityStart..at 的 @ 之前部分）替换为 ***，保留 '@' 及之后
    std::string out;
    out.reserve(url.size() + 2);
    out.append(url, 0, authorityStart);
    out.append("***");
    out.append(url, at, url.size() - at);
    return out;
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
    std::vector<unsigned char> header(static_cast<size_t>(std::min<std::uintmax_t>(fileSize, kMockHeaderScanBytes)));
    magicFile.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const size_t headerLen = static_cast<size_t>(std::max<std::streamsize>(0, magicFile.gcount()));
    if (!hasValidImageMagic(ext, header.data(), headerLen)) {
        if (headerLen < 3) {
            return setReason("MOCK_FILE_INCOMPLETE");
        }
        return setReason("MOCK_MAGIC_INVALID");
    }

    // 解码前尺寸边界：解析头部声明像素尺寸，任一维度超产品上界则拒绝，
    // 防止 imdecode 依据伪造头部分配巨大矩阵导致 OOM。解析失败放行，由解码后帧校验兜底。
    DeclaredDims dims;
    if (ext == "jpg" || ext == "jpeg") dims = parseJpegDims(header.data(), headerLen);
    else if (ext == "png") dims = parsePngDims(header.data(), headerLen);
    else if (ext == "bmp") dims = parseBmpDims(header.data(), headerLen);
    else if (ext == "webp") dims = parseWebpDims(header.data(), headerLen);
    if (dims.parsed && (dims.width > kMockMaxImageDimension || dims.height > kMockMaxImageDimension)) {
        return setReason("MOCK_IMAGE_DIMENSION_OVERFLOW");
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
        cap.set(cv::CAP_PROP_FRAME_WIDTH, rk_core::config::FRAME_WIDTH);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, rk_core::config::FRAME_HEIGHT);
        cap.set(cv::CAP_PROP_FPS, rk_core::config::TARGET_FPS);
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
    rklog::logInfo("MockMode", "open", "Mock file path=" + sanitizeUriForLog(filePath));

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
                std::cerr << "Failed to open image file: " << sanitizeUriForLog(filePath) << std::endl;
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
            auto chunk = std::make_unique<char[]>(chunkSize);
            while (file) {
                file.read(chunk.get(), chunkSize);
                std::streamsize count = file.gcount();
                if (count > 0) {
                    buffer.insert(buffer.end(), chunk.get(), chunk.get() + count);
                }
            }
            
            cv::Mat rawData(1, buffer.size(), CV_8UC1, buffer.data());
            staticFrame = cv::imdecode(rawData, cv::IMREAD_COLOR);

            if (staticFrame.empty()) {
                std::cerr << "Failed to decode image: " << sanitizeUriForLog(filePath) << std::endl;
                mockState = MockState::FAILED;
                setMockRejectReason("MOCK_DECODE_FAILED", "static_image_decode_failed");
                rklog::logError("MockMode", "open", "Failed to decode static image");
                return false;
            }
            // Resize to match config if needed
            if (staticFrame.cols != rk_core::config::FRAME_WIDTH || staticFrame.rows != rk_core::config::FRAME_HEIGHT) {
                cv::resize(staticFrame, staticFrame, cv::Size(rk_core::config::FRAME_WIDTH, rk_core::config::FRAME_HEIGHT));
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
            rklog::logInfo("MockMode", "tryOpen", "Opening: " + sanitizeUriForLog(pathOrUrl) + " timeoutMs=" + std::to_string(effectiveTimeout));
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
            rklog::logInfo("VideoManager", "open", "Connecting to: " + sanitizeUriForLog(primary));
            mockState = MockState::LOADING;
            if (!tryOpen(primary)) {
                rklog::logError("MockMode", "open", "Primary stream failed, state=FAILED");
                if (!backup.empty()) {
                    rklog::logInfo("MockMode", "open", "Trying backup: " + sanitizeUriForLog(backup));
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
#if defined(RK_HAVE_MPP) && RK_HAVE_MPP && !defined(_WIN32)
            if (mppRequested_.load()) mppDecoder = std::make_unique<MppDecoder>();
            if (mppDecoder && mppDecoder->init() && mppDecoder->open(filePath)) {
                useMppDecode = true;
                rklog::logInfo("MockMode", "open", "MPP hardware decoding enabled for: " + sanitizeUriForLog(filePath));
                // Also initialize cap as fallback in case MPP fails at runtime
                if (!tryOpen(filePath)) {
                    rklog::logWarn("MockMode", "open", "MPP backup cap.open failed (non-fatal, MPP will be primary)");
                } else {
                    try {
                        cap.set(cv::CAP_PROP_FRAME_WIDTH, rk_core::config::FRAME_WIDTH);
                        cap.set(cv::CAP_PROP_FRAME_HEIGHT, rk_core::config::FRAME_HEIGHT);
                        cap.set(cv::CAP_PROP_FPS, rk_core::config::TARGET_FPS);
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
                    cap.set(cv::CAP_PROP_FRAME_WIDTH, rk_core::config::FRAME_WIDTH);
                    cap.set(cv::CAP_PROP_FRAME_HEIGHT, rk_core::config::FRAME_HEIGHT);
                    cap.set(cv::CAP_PROP_FPS, rk_core::config::TARGET_FPS);
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
                cap.set(cv::CAP_PROP_FRAME_WIDTH, rk_core::config::FRAME_WIDTH);
                cap.set(cv::CAP_PROP_FRAME_HEIGHT, rk_core::config::FRAME_HEIGHT);
                cap.set(cv::CAP_PROP_FPS, rk_core::config::TARGET_FPS);
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
    mppEffective_.store(false);
    mppDecodedFrames_.store(0);
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        latestFrame.release();
        hasNewFrame = false;
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
    // 消费标记与发布共用一把锁，防止旧帧重复驱动识别或误清下一帧。
    hasNewFrame = false;
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
    int consecutiveReadFailures = 0;
    constexpr int kMaxConsecutiveReadFailures = 300;  // ~3s at 10ms sleep
    while (isRunning) {
        // HR-63: check external cancellation request
        if (cancelToken && cancelToken->load()) {
            isRunning = false;
            break;
        }
        // MPP hardware decoding branch (mock mode video files)
        if (isMockMode && useMppDecode && mppDecoder) {
            if (mppDecoder->read(tempFrame)) {
                consecutiveReadFailures = 0;
                std::lock_guard<std::mutex> lock(frameMutex);
                if (!tempFrame.empty()) {
                    const bool validMppFrame = tempFrame.cols > 0 && tempFrame.cols <= 4096 &&
                        tempFrame.rows > 0 && tempFrame.rows <= 4096 &&
                        (tempFrame.channels() == 3 || tempFrame.channels() == 4);
                    // 初始化成功不代表硬件产帧；仅成功解码且尺寸有效时记录运行证据。
                    if (validMppFrame) {
                        mppDecodedFrames_.fetch_add(1);
                        if (!mppEffective_.exchange(true)) {
                            rklog::logInfo("VideoManager", "captureLoop", rk_accel::formatSelfCheckLine(
                                {"mpp", mppRequested_.load(), true, "ok", "decoded_frames=1"}));
                        }
                    } else {
                        mppEffective_.store(false);
                    }
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
                // MPP decoding failed or EOS, fall back to VideoCapture
                rklog::logWarn("MockMode", "captureLoop", "MPP decode failed, falling back to VideoCapture");
                useMppDecode = false;
                mppEffective_.store(false);
                rklog::logInfo("VideoManager", "captureLoop", rk_accel::formatSelfCheckLine(
                    {"mpp", mppRequested_.load(), false, "runtime_init_failed", "decoder_fallback=opencv"}));
                mppDecoder.reset();
                if (!cap.isOpened()) {
                    cap.open(mockFilePath);
                }
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
                consecutiveReadFailures = 0;
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
                    if (tempFrame.cols != rk_core::config::FRAME_WIDTH || tempFrame.rows != rk_core::config::FRAME_HEIGHT) {
                        cv::resize(tempFrame, tempFrame, cv::Size(rk_core::config::FRAME_WIDTH, rk_core::config::FRAME_HEIGHT));
                    }
                    // Swap buffers instead of copy if possible, or move
                    // Since tempFrame is reused, we must copy. 
                    // BUT: We can optimize by swapping if we had a pool. 
                    // For now, let's keep copyTo but ensure latestFrame allocation is reused.
                    tempFrame.copyTo(latestFrame); 
                    hasNewFrame = true;
                }
            } else {
                consecutiveReadFailures++;
                if (!cap.isOpened() || consecutiveReadFailures >= kMaxConsecutiveReadFailures) {
                    rklog::logError("VideoManager", "captureLoop",
                        "Camera read failed " + std::to_string(consecutiveReadFailures) +
                        " consecutive times, stopping capture");
                    if (isMockMode) {
                        if (cap.isOpened()) {
                            cap.set(cv::CAP_PROP_POS_FRAMES, 0);
                            consecutiveReadFailures = 0;
                        } else {
                            isRunning = false;
                        }
                    } else {
                        isRunning = false;
                    }
                    continue;
                }
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
