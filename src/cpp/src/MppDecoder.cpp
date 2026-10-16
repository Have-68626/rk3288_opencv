#include "MppDecoder.h"
#include "NativeLog.h"

#if defined(RK_HAVE_MPP) && RK_HAVE_MPP && !defined(_WIN32)
#include <rk_mpi.h>
#include <mpp_err.h>
#endif

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdio>
#include <vector>

struct MppDecoder::MppState {
#if defined(RK_HAVE_MPP) && RK_HAVE_MPP && !defined(_WIN32)
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

static constexpr size_t kChunkSize = 1024 * 1024;  // 1MB per read

static int64_t getFileSize(std::FILE* fh) {
    int64_t pos = std::ftell(fh);
    std::fseek(fh, 0, SEEK_END);
    int64_t size = std::ftell(fh);
    std::fseek(fh, pos, SEEK_SET);
    return size;
}

MppDecoder::MppDecoder()
    : mpp_(std::make_unique<MppState>()) {
}

MppDecoder::~MppDecoder() {
    close();
}

bool MppDecoder::convertNv12ToBgr(const uint8_t* data, int width, int height,
                                 int horizontalStride, int verticalStride, cv::Mat& outBgr) {
    if (!data || width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        width % 2 != 0 || height % 2 != 0 || horizontalStride < width || verticalStride < height) {
        return false;
    }
    // UV 从完整 Y 平面之后开始；两个可见平面各自保留行 stride，避免把 padding 当像素。
    const auto uvOffset = static_cast<std::size_t>(horizontalStride) * verticalStride;
    cv::Mat y(height, width, CV_8UC1, const_cast<uint8_t*>(data), horizontalStride);
    cv::Mat uv(height / 2, width / 2, CV_8UC2, const_cast<uint8_t*>(data + uvOffset), horizontalStride);
    cv::cvtColorTwoPlane(y, uv, outBgr, cv::COLOR_YUV2BGR_NV12);
    return !outBgr.empty();
}

bool MppDecoder::init() {
#if defined(RK_HAVE_MPP) && RK_HAVE_MPP && !defined(_WIN32)
    MPP_RET ret = mpp_create(&mpp_->ctx, &mpp_->mpi);
    if (ret != MPP_OK) {
        rklog::logError("MppDecoder", "init", "mpp_create failed: ret=" + std::to_string(ret));
        return false;
    }

    // Enable byte-stream parser split mode: MPP will find NALU boundaries internally,
    // so callers can feed data in arbitrary-sized chunks without manual splitting.
    {
        RK_U32 need_split = 1;
        ret = mpp_->mpi->control(mpp_->ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &need_split);
        if (ret != MPP_OK) {
            rklog::logWarn("MppDecoder", "init", "MPP_DEC_SET_PARSER_SPLIT_MODE failed: ret=" + std::to_string(ret));
        } else {
            rklog::logInfo("MppDecoder", "init", "MPP byte-stream split mode enabled");
        }
    }

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

#if defined(RK_HAVE_MPP) && RK_HAVE_MPP && !defined(_WIN32)
    mpp_->fileHandle = std::fopen(filePath.c_str(), "rb");
    if (!mpp_->fileHandle) {
        rklog::logError("MppDecoder", "open", "Failed to open file: " + filePath);
        return false;
    }

    fileSize_ = getFileSize(mpp_->fileHandle);
    fileReadOffset_ = 0;
    mpp_->eos = false;
    reachedEos_ = false;
    opened_ = true;

    rklog::logInfo("MppDecoder", "open",
        "Streaming: " + filePath + " (" + std::to_string(fileSize_) + " bytes)");
    return true;
#else
    (void)filePath;
    return false;
#endif
}

bool MppDecoder::read(cv::Mat& outBgr) {
    if (!opened_ || !inited_) return false;

#if defined(RK_HAVE_MPP) && RK_HAVE_MPP && !defined(_WIN32)
    if (hasFrame_) {
        if (!latestBgr_.empty()) {
            latestBgr_.copyTo(outBgr);
        }
        hasFrame_ = false;
        return !latestBgr_.empty();
    }

    // Decode loop: read chunk, feed to MPP, collect frames
    while (true) {
        MppPacket packet = nullptr;
        std::shared_ptr<std::vector<uint8_t>> buf;
        size_t bytesRead = 0;
        int64_t readPos = 0;
        if (mpp_->fileHandle && !mpp_->eos) {
            readPos = std::ftell(mpp_->fileHandle);
            buf = std::make_shared<std::vector<uint8_t>>(kChunkSize);
            bytesRead = std::fread(buf->data(), 1, kChunkSize, mpp_->fileHandle);
            if (bytesRead > 0) {
                fileReadOffset_ += static_cast<int64_t>(bytesRead);
            }
            if (std::feof(mpp_->fileHandle) || bytesRead == 0) {
                mpp_->eos = true;
            }
        }

        // set_data 不会同步 pos；SDK 从 pos 复制数据，必须一次性初始化四个数据字段。
        if (mpp_packet_init(&packet, bytesRead ? buf->data() : nullptr, bytesRead) != MPP_OK) return false;
        if (bytesRead) pendingBufs_.push_back(std::move(buf));

        if (mpp_->eos) {
            mpp_packet_set_eos(packet);
        }

        MPP_RET putRet = mpp_->mpi->decode_put_packet(mpp_->ctx, packet);

        if (putRet == MPP_ERR_BUFFER_FULL) {
            // MPP input buffer full — rewind file to pre-read position, drain frames, retry
            mpp_packet_deinit(&packet);
            pendingBufs_.clear();
            if (mpp_->fileHandle && readPos >= 0) {
                fileReadOffset_ -= (std::ftell(mpp_->fileHandle) - readPos);
                std::fseek(mpp_->fileHandle, readPos, SEEK_SET);
                mpp_->eos = false;
            }
            while (true) {
                MppFrame frame = nullptr;
                MPP_RET ret = mpp_->mpi->decode_get_frame(mpp_->ctx, &frame);
                if (ret != MPP_OK || !frame) break;
                bool eos = mpp_frame_get_eos(frame);
                mpp_frame_deinit(&frame);
                if (eos) goto handle_eos;
            }
            continue;
        }

        // 未设置 MppBuffer 时 SDK 在 decode_put_packet 返回前已同步复制裸数据。
        mpp_packet_deinit(&packet);
        pendingBufs_.clear();

        if (putRet != MPP_OK) {
            if (mpp_->eos) break;
            continue;
        }

        // Collect all available frames from this packet
        bool gotFrame = false;
        while (true) {
            MppFrame frame = nullptr;
            MPP_RET ret = mpp_->mpi->decode_get_frame(mpp_->ctx, &frame);
            if (ret != MPP_OK || !frame) break;

            if (mpp_frame_get_info_change(frame) || mpp_frame_get_eos(frame)) {
                bool eos = mpp_frame_get_eos(frame);
                mpp_frame_deinit(&frame);
                if (eos) {
                    goto handle_eos;
                }
                continue;
            }

            int w = mpp_frame_get_width(frame);
            int h = mpp_frame_get_height(frame);
            int horStride = mpp_frame_get_hor_stride(frame);
            int verStride = mpp_frame_get_ver_stride(frame);

            if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
                MppBuffer buf = mpp_frame_get_buffer(frame);
                if (buf) {
                    uint8_t* data = static_cast<uint8_t*>(mpp_buffer_get_ptr(buf));
                    cv::Mat bgr;
                    convertNv12ToBgr(data, w, h, horStride, verStride, bgr);
                    if (!bgr.empty()) {
                        bgr.copyTo(latestBgr_);
                        hasFrame_ = true;
                        gotFrame = true;
                    }
                }
            }
            mpp_frame_deinit(&frame);
            if (gotFrame) break;
        }

        if (hasFrame_) {
            if (!latestBgr_.empty()) {
                latestBgr_.copyTo(outBgr);
                hasFrame_ = false;
                return true;
            }
            return false;
        }

        if (mpp_->eos) break;
    }

handle_eos:
    // Rewind for looped playback
    if (mpp_->fileHandle && fileSize_ > 0) {
        // Reset MPP decoder state before rewinding, otherwise stale EOS/internal state
        // causes decode_put_packet to reject new data
        if (mpp_->mpi) {
            mpp_->mpi->reset(mpp_->ctx);
        }
        std::fseek(mpp_->fileHandle, 0, SEEK_SET);
        fileReadOffset_ = 0;
        mpp_->eos = false;
        reachedEos_ = true;
        rklog::logInfo("MppDecoder", "read", "EOS reached, rewound for looped playback");
    }
    return false;
#else
    return false;
#endif
}

void MppDecoder::close() {
#if defined(RK_HAVE_MPP) && RK_HAVE_MPP && !defined(_WIN32)
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
    reachedEos_ = false;
    fileReadOffset_ = 0;
    fileSize_ = 0;
    pendingBufs_.clear();
    latestBgr_ = cv::Mat();
    rklog::logInfo("MppDecoder", "close", "MPP decoder closed");
}

bool MppDecoder::isOpened() const {
    return opened_;
}
