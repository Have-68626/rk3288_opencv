#pragma once

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

    /**
     * @brief Initialize MPP decoder context.
     * @return true if MPP library is available and initialized.
     */
    bool init();

    /**
     * @brief Open a video file for MPP hardware decoding.
     * @param filePath Path to H.264/H.265/MPEG4 video file.
     * @return true if file opened and decoder configured.
     */
    bool open(const std::string& filePath);

    /**
     * @brief Read next decoded frame.
     * @param outBgr Output BGR frame.
     * @return true if a frame is available.
     */
    bool read(cv::Mat& outBgr);

    /**
     * @brief Close decoder and release resources.
     */
    void close();

    /**
     * @brief Check if MPP is currently running.
     */
    bool isOpened() const;

private:
    bool decodePacket(std::vector<uint8_t>& packetData);

    // Forward declaration for MPP internal state
    struct MppState;
    std::unique_ptr<MppState> mpp_;
    bool inited_ = false;
    bool opened_ = false;
    bool hasFrame_ = false;
    cv::Mat latestBgr_;
    std::vector<uint8_t> packetBuffer_;
    MppDecoderConfig cfg_{};
};
