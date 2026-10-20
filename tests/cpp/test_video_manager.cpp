#include "VideoManager.h"
#include "MppDecoder.h"
#include "NativeLog.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace rk_core;

namespace rk_core {
struct VideoManagerTestPeer {
    static void publish(VideoManager& manager, const cv::Mat& frame) {
        std::lock_guard<std::mutex> lock(manager.frameMutex);
        frame.copyTo(manager.latestFrame);
        manager.hasNewFrame = true;
    }
};
}

bool test_video_manager_consumes_latest_frame_once() {
    VideoManager manager;
    cv::Mat output;
    if (manager.getLatestFrame(output)) return false;
    VideoManagerTestPeer::publish(manager, cv::Mat(2, 2, CV_8UC3, cv::Scalar(11, 22, 33)));
    if (!manager.getLatestFrame(output) || output.at<cv::Vec3b>(0, 0)[0] != 11) return false;
    if (manager.getLatestFrame(output)) return false;
    // 新发布必须恢复可读，防止只把标记永久清零的伪修复。
    VideoManagerTestPeer::publish(manager, cv::Mat(2, 2, CV_8UC3, cv::Scalar(44, 55, 66)));
    if (!manager.getLatestFrame(output) || output.at<cv::Vec3b>(0, 0)[0] != 44) return false;
    return !manager.getLatestFrame(output);
}

bool test_video_manager_close_discards_pending_frame() {
    VideoManager manager;
    VideoManagerTestPeer::publish(manager, cv::Mat(2, 2, CV_8UC3, cv::Scalar(11, 22, 33)));
    manager.close();
    cv::Mat output;
    return !manager.getLatestFrame(output);
}

bool test_mpp_nv12_conversion_preserves_uv_and_strides() {
    for (const int stride : {4, 8}) {
        const int width = 4, height = 4, verticalStride = stride;
        std::vector<uint8_t> pixels(static_cast<std::size_t>(stride * verticalStride * 3 / 2), 0);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) pixels[y * stride + x] = 81;
        for (int y = 0; y < height / 2; ++y)
            for (int x = 0; x < width; x += 2) {
                pixels[stride * verticalStride + y * stride + x] = 90;
                pixels[stride * verticalStride + y * stride + x + 1] = 240;
            }
        cv::Mat output;
        if (!MppDecoder::convertNv12ToBgr(pixels.data(), width, height, stride, verticalStride, output)) return false;
        if (output.rows != height || output.cols != width || output.type() != CV_8UC3) return false;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const auto value = output.at<cv::Vec3b>(y, x);
                if (value[2] < 250 || value[0] > 5 || value[1] > 5) return false;
            }
    }
    return true;
}

bool test_mpp_nv12_conversion_rejects_invalid_layout() {
    std::vector<uint8_t> pixels(48, 128);
    cv::Mat output;
    return !MppDecoder::convertNv12ToBgr(nullptr, 4, 4, 4, 4, output) &&
        !MppDecoder::convertNv12ToBgr(pixels.data(), 3, 4, 4, 4, output) &&
        !MppDecoder::convertNv12ToBgr(pixels.data(), 4, 3, 4, 4, output) &&
        !MppDecoder::convertNv12ToBgr(pixels.data(), 4, 4, 2, 4, output) &&
        !MppDecoder::convertNv12ToBgr(pixels.data(), 4, 4, 4, 2, output);
}

#if defined(RK_ACCEL_TEST_MPP_STUB)
// 驱动替身仅提供合成 NV12，packet 数据生命周期仍由 SDK 原始 mpp_packet.c 执行。
void rkTestMppAllowFrames(bool value);
unsigned rkTestMppCreates();
bool rkTestMppPacketValid();
bool test_video_manager_mpp_requires_request_and_decoded_frame() {
    const auto path = std::filesystem::temp_directory_path() /
        ("rk_mpp_synthetic_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    struct RemoveSynthetic {
        std::filesystem::path path;
        ~RemoveSynthetic() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{path};
    { std::ofstream output(path, std::ios::binary); output << "test synthetic packet"; }
    const unsigned before = rkTestMppCreates();
    {
        VideoManager disabled;
        disabled.setUseMpp(false);
        disabled.open(path.string());
        disabled.close();
        if (rkTestMppCreates() != before) return false;
    }
    rkTestMppAllowFrames(false);
    VideoManager enabled;
    enabled.setUseMpp(true);
    if (!enabled.open(path.string()) || !enabled.mppRequested() || enabled.mppEffective()) return false;
    rkTestMppAllowFrames(true);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!enabled.mppEffective() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    const bool actualFrame = enabled.mppEffective() && enabled.mppDecodedFrames() > 0;
    cv::Mat decoded;
    const bool readable = enabled.getLatestFrame(decoded) && decoded.rows == 4 && decoded.cols == 4;
    enabled.close();
    return actualFrame && readable && !enabled.mppEffective() && enabled.mppDecodedFrames() == 0 && rkTestMppPacketValid();
}
#endif

bool test_video_manager_is_url_source_valid_urls() {
    if (!VideoManager::isUrlSource("http://example.com/video.mp4")) return false;
    if (!VideoManager::isUrlSource("https://example.com/video.mp4")) return false;
    if (!VideoManager::isUrlSource("rtsp://192.168.1.100:554/stream")) return false;
    if (!VideoManager::isUrlSource("rtmp://live.twitch.tv/app/stream_key")) return false;
    return true;
}

bool test_video_manager_is_url_source_local_paths() {
    if (VideoManager::isUrlSource("/home/user/video.mp4")) return false;
    if (VideoManager::isUrlSource("C:\\Users\\user\\video.mp4")) return false;
    if (VideoManager::isUrlSource("video.mp4")) return false;
    if (VideoManager::isUrlSource("./video.mp4")) return false;
    if (VideoManager::isUrlSource("../video.mp4")) return false;
    return true;
}

bool test_video_manager_is_url_source_edge_cases() {
    if (VideoManager::isUrlSource("")) return false;
    if (VideoManager::isUrlSource("http")) return false; // Shorter than prefix
    if (VideoManager::isUrlSource("http:")) return false;
    if (VideoManager::isUrlSource("http:/")) return false;

    // Prefix not at the beginning
    if (VideoManager::isUrlSource(" file://http://example.com")) return false;
    if (VideoManager::isUrlSource("my_http://example.com")) return false;
    if (VideoManager::isUrlSource("/some/path/http://example.com")) return false;

    // Case sensitivity (current implementation is case-sensitive and only checks lowercase)
    // If the requirements change to case-insensitive, this test will fail and need to be updated.
    if (VideoManager::isUrlSource("HTTP://example.com/video.mp4")) return false;
    if (VideoManager::isUrlSource("RTSP://192.168.1.100/stream")) return false;

    return true;
}

// ── S-SEC-B07-002：URI userinfo 日志脱敏 ─────────────────────────────
// RED 阶段：桩实现直传 URL，以下"无原值"断言失败（userinfo 原值出现在输出中，证明泄漏）。
// GREEN 阶段：sanitizeUriForLog 掩码 userinfo 段后，同一断言翻转为"无原值"。

// 负面用例：含 userinfo 的 RTSP URL 脱敏后不得出现 user 与 pass 原值。
bool test_video_manager_sanitize_uri_hides_userinfo() {
    const std::string url = "rtsp://alice:secret@192.168.1.10:554/stream";
    const std::string out = VideoManager::sanitizeUriForLog(url);
    if (out.find("alice") != std::string::npos) return false;   // user 原值不可见
    if (out.find("secret") != std::string::npos) return false;   // pass 原值不可见
    return true;
}

// 正向断言：保留 scheme://host:port 可读性，仅掩 userinfo 段。
bool test_video_manager_sanitize_uri_keeps_host_port() {
    const std::string out = VideoManager::sanitizeUriForLog("rtsp://alice:secret@192.168.1.10:554/stream");
    return out == "rtsp://***@192.168.1.10:554/stream";
}

// HTTP 变体（含 user 无 pass）与 https 掩码形态。
bool test_video_manager_sanitize_uri_http_variants() {
    if (VideoManager::sanitizeUriForLog("http://user@example.com:8080/path") !=
        "http://***@example.com:8080/path") return false;
    if (VideoManager::sanitizeUriForLog("https://bob:pass@example.com/video.mp4") !=
        "https://***@example.com/video.mp4") return false;
    return true;
}

// 非 URL（本地路径 / 无 scheme）必须原样返回，不得误伤。
bool test_video_manager_sanitize_uri_non_url_passthrough() {
    if (VideoManager::sanitizeUriForLog("/home/user/video.mp4") != "/home/user/video.mp4") return false;
    if (VideoManager::sanitizeUriForLog("C:\\Users\\user\\video.mp4") != "C:\\Users\\user\\video.mp4") return false;
    if (VideoManager::sanitizeUriForLog("video.mp4") != "video.mp4") return false;
    if (VideoManager::sanitizeUriForLog("") != "") return false;
    // 含 @ 但非 URL 的路径不应被当作 userinfo 掩码
    if (VideoManager::sanitizeUriForLog("dir/file@name.txt") != "dir/file@name.txt") return false;
    return true;
}

// 无 userinfo 的 URL 原样返回。
bool test_video_manager_sanitize_uri_no_userinfo() {
    if (VideoManager::sanitizeUriForLog("rtsp://192.168.1.100:554/stream") !=
        "rtsp://192.168.1.100:554/stream") return false;
    if (VideoManager::sanitizeUriForLog("http://example.com:8080/path") !=
        "http://example.com:8080/path") return false;
    return true;
}

// 集成断言：经 VideoManager 脱敏 seam 的消息经 NativeLog 文件落盘后无 userinfo 原值。
bool test_video_manager_uri_log_sink_no_userinfo() {
    const std::string suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("rk_vm_uri_log" + suffix);
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "rk_vm_uri.log";

    rklog::setLogDirs(dir.string(), "", "rk_vm_uri.log");
    // 模拟 VideoManager::open 的日志调用点：日志消息使用脱敏后 URL。
    rklog::logInfo("MockMode", "tryOpen",
                   "Opening: " + VideoManager::sanitizeUriForLog("rtsp://alice:secret@192.168.1.10:554/stream"));
    rklog::setLogDirs("", "", "");  // 关闭 FD 避免占用日志文件

    std::string content;
    {
        std::ifstream in(file, std::ios::binary);  // 局部作用域：读取后即关闭，Windows 下
        std::ostringstream ss;                      // 移除目录前若句柄仍打开会抛异常
        ss << in.rdbuf();
        content = ss.str();
    }
    std::filesystem::remove_all(dir);

    if (content.find("alice") != std::string::npos) return false;   // user 原值不得落盘
    if (content.find("secret") != std::string::npos) return false;   // pass 原值不得落盘
    if (content.find("rtsp://***@192.168.1.10:554/stream") == std::string::npos) return false;
    return true;
}
