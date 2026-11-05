#include "rk_win/CameraSource.h"
#include "rk_win/Platform.h"

#ifdef _WIN32
#include "rk_win/MfCamera.h"
#else
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#endif

namespace rk_win {

#ifdef _WIN32

std::unique_ptr<ICameraSource> createCameraSource() {
    return std::make_unique<MfCamera>();
}

std::vector<CameraDevice> enumerateCameraDevices() {
    return MfCamera::enumerateDevices();
}

#else

namespace {

#if RK_WIN_HAS_OPENCV
// Linux/Android：OpenCV VideoCapture 的 V4L2 后端。只实现 ICameraSource 的最小语义。
class OpenCvV4l2Camera : public ICameraSource {
public:
    CameraOpenResult open(const std::wstring& deviceId, int width, int height, int fps) override {
        close();
        CameraOpenResult r;
        const std::string dev = platform::utf8FromWide(deviceId);
        if (!cap_.open(dev, cv::CAP_V4L2)) {
            r.ok = false;
            r.category = ErrorCategory::BackendFailure;
            r.code = "open_failed";
            r.message = "无法打开 V4L2 设备: " + dev;
            return r;
        }
        if (width > 0) cap_.set(cv::CAP_PROP_FRAME_WIDTH, width);
        if (height > 0) cap_.set(cv::CAP_PROP_FRAME_HEIGHT, height);
        if (fps > 0) cap_.set(cv::CAP_PROP_FPS, fps);
        fmt_.width = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
        fmt_.height = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
        fmt_.fps = static_cast<int>(cap_.get(cv::CAP_PROP_FPS));
        fmt_.subtype = L"BGR24";
        dev_.deviceId = deviceId;
        dev_.name = L"V4L2 " + deviceId;
        dev_.formats = {fmt_};
        r.ok = true;
        r.category = ErrorCategory::None;
        r.code = "ok";
        return r;
    }

    void close() override {
        if (cap_.isOpened()) cap_.release();
        open_ = false;
    }

    CameraOpenResult readFrameBgr(cv::Mat& outBgr, std::uint64_t& outTimestamp100ns) override {
        CameraOpenResult r;
        if (!cap_.isOpened()) {
            r.category = ErrorCategory::BackendFailure;
            r.code = "not_open";
            r.message = "相机未打开";
            return r;
        }
        cv::Mat frame;
        if (!cap_.read(frame) || frame.empty()) {
            r.category = ErrorCategory::BackendFailure;
            r.code = "read_failed";
            r.message = "V4L2 读取帧失败";
            return r;
        }
        outBgr = frame;
        // 时间戳：相对本进程首次读取的单调时钟，单位 100ns（与 Windows MF 样本时间同量纲，
        // 但基准是进程启动而非系统时间）。当前只用于渲染/日志展示。
        static const auto epoch = std::chrono::steady_clock::now();
        const auto delta = std::chrono::steady_clock::now() - epoch;
        outTimestamp100ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count() / 100);
        open_ = true;
        r.ok = true;
        r.category = ErrorCategory::None;
        r.code = "ok";
        return r;
    }

    bool isOpen() const override { return cap_.isOpened(); }
    CameraDevice currentDevice() const override { return dev_; }
    CameraFormat currentFormat() const override { return fmt_; }

private:
    cv::VideoCapture cap_;
    CameraDevice dev_;
    CameraFormat fmt_;
    bool open_ = false;
};
#endif  // RK_WIN_HAS_OPENCV

}  // namespace

std::unique_ptr<ICameraSource> createCameraSource() {
#if RK_WIN_HAS_OPENCV
    return std::make_unique<OpenCvV4l2Camera>();
#else
    // 无 OpenCV 构建：返回显式失败的桩，避免 FramePipeline 解引用空指针。
    class NullCamera final : public ICameraSource {
    public:
        CameraOpenResult open(const std::wstring&, int, int, int) override {
            return {false, ErrorCategory::BackendFailure, "no_camera_backend", "本次构建未启用 OpenCV 视频后端"};
        }
        void close() override {}
        CameraOpenResult readFrameBgr(cv::Mat&, std::uint64_t&) override {
            return {false, ErrorCategory::BackendFailure, "no_camera_backend", "本次构建未启用 OpenCV 视频后端"};
        }
        bool isOpen() const override { return false; }
        CameraDevice currentDevice() const override { return {}; }
        CameraFormat currentFormat() const override { return {}; }
    };
    return std::make_unique<NullCamera>();
#endif
}

std::vector<CameraDevice> enumerateCameraDevices() {
    std::vector<CameraDevice> devices;
    std::error_code ec;
    for (int i = 0; i < 64; ++i) {
        const std::filesystem::path p = "/dev/video" + std::to_string(i);
        if (!std::filesystem::exists(p, ec)) continue;
        CameraDevice d;
        d.deviceId = platform::wideFromUtf8(p.string());
        d.name = L"V4L2 " + d.deviceId;
        devices.push_back(std::move(d));
    }
    return devices;
}

#endif  // _WIN32

}  // namespace rk_win
