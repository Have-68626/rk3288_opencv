#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

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

    // 硬件帧的 UV 起点由垂直 stride 决定，不能先按可见高度裁剪整张 NV12。
    static bool convertNv12ToBgr(const uint8_t* data, int width, int height,
                                 int horizontalStride, int verticalStride, cv::Mat& outBgr);

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
