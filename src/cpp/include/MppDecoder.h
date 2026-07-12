#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#if __has_include(<opencv2/core.hpp>) && !defined(RK_SKIP_OPENCV)
#include <opencv2/core.hpp>
#else
namespace cv {
class Mat {
public:
    Mat() = default;
    bool empty() const { return true; }
    void copyTo(Mat&) const {}
};
}
#endif

struct MppDecoderConfig {
    int inputW = 0;
    int inputH = 0;
    int timeoutMs = 10000;
};

class MppDecoder {
public:
    MppDecoder();
    ~MppDecoder();

    bool init();
    bool open(const std::string& filePath);
    bool read(cv::Mat& outBgr);
    void close();
    bool isOpened() const;

private:
    struct MppState;
    std::unique_ptr<MppState> mpp_;
    bool inited_ = false;
    bool opened_ = false;
    bool hasFrame_ = false;
    bool reachedEos_ = false;
    cv::Mat latestBgr_;
    std::list<std::shared_ptr<std::vector<uint8_t>>> pendingBufs_;
    int64_t fileReadOffset_ = 0;
    int64_t fileSize_ = 0;
    MppDecoderConfig cfg_{};
};
