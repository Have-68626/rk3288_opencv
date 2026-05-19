#include "MppDecoder.h"

struct MppDecoder::MppState {};

MppDecoder::MppDecoder()
    : mpp_(std::make_unique<MppState>()) {
}

MppDecoder::~MppDecoder() = default;

bool MppDecoder::init() {
    inited_ = false;
    return false;
}

bool MppDecoder::open(const std::string& filePath) {
    (void)filePath;
    opened_ = false;
    return false;
}

bool MppDecoder::read(cv::Mat& outBgr) {
    (void)outBgr;
    return false;
}

void MppDecoder::close() {
    opened_ = false;
    hasFrame_ = false;
    reachedEos_ = false;
    latestBgr_.release();
}

bool MppDecoder::isOpened() const {
    return opened_;
}

