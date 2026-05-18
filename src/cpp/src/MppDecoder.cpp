#include "MppDecoder.h"
#include "NativeLog.h"

#if defined(RK_HAVE_MPP) && RK_HAVE_MPP
#include <rk_mpi.h>
#include <mpp.h>
#include <mpp_err.h>
#endif

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

struct MppDecoder::MppState {
#if defined(RK_HAVE_MPP) && RK_HAVE_MPP
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppBufferGroup frameGroup = nullptr;
    MppPacket packet = nullptr;
    MppFrame frame = nullptr;
    MppCodingType codingType = MPP_VIDEO_CodingAVC;
    bool eos = false;
    std::FILE* fileHandle = nullptr;
#endif
};

MppDecoder::MppDecoder()
    : mpp_(std::make_unique<MppState>()) {
}

MppDecoder::~MppDecoder() {
    close();
}

bool MppDecoder::init() {
#if defined(RK_HAVE_MPP) && RK_HAVE_MPP
    MPP_RET ret = mpp_create(&mpp_->ctx, &mpp_->mpi);
    if (ret != MPP_OK) {
        rklog::logError("MppDecoder", "init", "mpp_create failed: ret=" + std::to_string(ret));
        return false;
    }

    // Initialize decoder with H.264 (most common for mock files)
    ret = mpp_init(mpp_->ctx, MPP_CTX_DEC, mpp_->codingType);
    if (ret != MPP_OK) {
        rklog::logError("MppDecoder", "init", "mpp_init failed: ret=" + std::to_string(ret));
        mpp_destroy(mpp_->ctx);
        mpp_->ctx = nullptr;
        return false;
    }

    inited_ = true;
    rklog::logInfo("MppDecoder", "init", "MPP decoder initialized successfully");
    return true;
#else
    rklog::logWarn("MppDecoder", "init", "MPP not available (RK_HAVE_MPP=0)");
    return false;
#endif
}

bool MppDecoder::open(const std::string& filePath) {
    if (!inited_) {
        if (!init()) return false;
    }

#if defined(RK_HAVE_MPP) && RK_HAVE_MPP
    mpp_->fileHandle = std::fopen(filePath.c_str(), "rb");
    if (!mpp_->fileHandle) {
        rklog::logError("MppDecoder", "open", "Failed to open file: " + filePath);
        return false;
    }

    // Read whole file into packet buffer (for looped playback)
    std::fseek(mpp_->fileHandle, 0, SEEK_END);
    long fileSize = std::ftell(mpp_->fileHandle);
    std::fseek(mpp_->fileHandle, 0, SEEK_SET);

    packetBuffer_.resize(fileSize);
    size_t bytesRead = std::fread(packetBuffer_.data(), 1, fileSize, mpp_->fileHandle);
    if (bytesRead != static_cast<size_t>(fileSize)) {
        rklog::logWarn("MppDecoder", "open", "Read " + std::to_string(bytesRead) +
            "/" + std::to_string(fileSize) + " bytes");
    }

    std::fclose(mpp_->fileHandle);
    mpp_->fileHandle = nullptr;
    mpp_->eos = false;
    opened_ = true;
    hasFrame_ = false;

    rklog::logInfo("MppDecoder", "open", "Loaded " + std::to_string(packetBuffer_.size()) +
        " bytes from " + filePath);
    return true;
#else
    (void)filePath;
    return false;
#endif
}

bool MppDecoder::read(cv::Mat& outBgr) {
    if (!opened_ || !inited_) return false;

#if defined(RK_HAVE_MPP) && RK_HAVE_MPP
    // If we already have an unconsumed frame, return it
    if (hasFrame_) {
        if (!latestBgr_.empty()) {
            latestBgr_.copyTo(outBgr);
        }
        hasFrame_ = false;
        return !latestBgr_.empty();
    }

    // Decode loop: send packets until we get a frame or EOS
    size_t offset = 0;
    while (offset < packetBuffer_.size() || mpp_->eos) {
        MppPacket packet = nullptr;

        if (offset < packetBuffer_.size()) {
            size_t chunkSize = std::min<size_t>(packetBuffer_.size() - offset, 1024 * 1024);
            mpp_packet_new(&packet);
            mpp_packet_set_data(packet, packetBuffer_.data() + offset);
            mpp_packet_set_size(packet, chunkSize);
            mpp_packet_set_length(packet, chunkSize);
            offset += chunkSize;
        } else if (mpp_->eos) {
            // Flush remaining frames with null packet
            mpp_packet_new(&packet);
            mpp_packet_set_eos(packet);
        } else {
            break;
        }

        MPP_RET ret = mpp_->mpi->decode_put_packet(mpp_->ctx, packet);
        if (ret != MPP_OK && ret != MPP_ERR_BUFFER_FULL) {
            mpp_packet_destroy(packet);
            break;
        }

        // Try to get decoded frame
        MppFrame frame = nullptr;
        ret = mpp_->mpi->decode_get_frame(mpp_->ctx, &frame);
        if (ret == MPP_OK && frame) {
            if (mpp_frame_get_info_change(frame) || mpp_frame_get_eos(frame)) {
                if (mpp_frame_get_eos(frame)) {
                    // End of stream reached
                    if (offset >= packetBuffer_.size()) {
                        // Rewind for looped playback
                        offset = 0;
                        rklog::logInfo("MppDecoder", "read", "EOS reached, rewinding for loop");
                    }
                    mpp_frame_deinit(&frame);
                    mpp_packet_destroy(packet);
                    return false;
                }
                mpp_frame_deinit(&frame);
                mpp_packet_destroy(packet);
                continue;
            }

            // Convert MppFrame (NV12) to cv::Mat (BGR)
            int w = mpp_frame_get_width(frame);
            int h = mpp_frame_get_height(frame);
            int horStride = mpp_frame_get_hor_stride(frame);
            int verStride = mpp_frame_get_ver_stride(frame);

            if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
                MppBuffer buf = mpp_frame_get_buffer(frame);
                if (buf) {
                    uint8_t* data = static_cast<uint8_t*>(mpp_buffer_get_ptr(buf));
                    cv::Mat nv12(verStride * 3 / 2, horStride, CV_8UC1, data);
                    cv::Mat bgr;
                    cv::cvtColor(nv12(cv::Rect(0, 0, w, h)), bgr, cv::COLOR_YUV2BGR_NV12);
                    if (!bgr.empty()) {
                        bgr.copyTo(latestBgr_);
                        hasFrame_ = true;
                    }
                }
            }

            mpp_frame_deinit(&frame);

            if (hasFrame_) {
                mpp_packet_destroy(packet);
                if (!latestBgr_.empty()) {
                    latestBgr_.copyTo(outBgr);
                    hasFrame_ = false;
                    return true;
                }
                return false;
            }
        }

        mpp_packet_destroy(packet);
    }

    return false;
#else
    return false;
#endif
}

void MppDecoder::close() {
#if defined(RK_HAVE_MPP) && RK_HAVE_MPP
    if (mpp_->fileHandle) {
        std::fclose(mpp_->fileHandle);
        mpp_->fileHandle = nullptr;
    }
    if (mpp_->ctx) {
        if (mpp_->mpi) {
            mpp_->mpi->reset(mpp_->ctx);
        }
        mpp_destroy(mpp_->ctx);
        mpp_->ctx = nullptr;
        mpp_->mpi = nullptr;
    }
#endif
    opened_ = false;
    inited_ = false;
    hasFrame_ = false;
    packetBuffer_.clear();
    latestBgr_ = cv::Mat();
    rklog::logInfo("MppDecoder", "close", "MPP decoder closed");
}

bool MppDecoder::isOpened() const {
    return opened_;
}
