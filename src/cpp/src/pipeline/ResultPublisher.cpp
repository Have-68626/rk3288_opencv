#include "pipeline/ResultPublisher.h"
#include <algorithm>
#include <chrono>

namespace pipeline {

ResultPublisher::ResultPublisher() {}

void ResultPublisher::setCallback(TextCallback cb) {
    callback_ = std::move(cb);
}

void ResultPublisher::publish(const FrameOutcome& outcome) {
    long long now = outcome.events.empty() ? 0 : outcome.events[0].timestampMs;
    if (now == 0) {
        // 尝试从 stats 推断或使用当前时间
        now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    if (callback_) {
        if (outcome.tracks.empty()) {
            if (now - lastNoFaceMs_ > 2000) {
                lastNoFaceMs_ = now;
                callback_("NO_FACE");
            }
        } else if (outcome.tracks.size() > 1) {
            if (now - lastMultiMs_ > 650) {
                lastMultiMs_ = now;
                std::string msg = "FACES " + std::to_string(outcome.tracks.size());
                for (const auto& t : outcome.tracks) {
                    msg += " T" + std::to_string(t.trackId) + "=" + t.stableId;
                    if (t.stableId != "Unknown") {
                        msg += "(" + std::to_string(static_cast<int>(t.stableConfidence * 100)) + "%)";
                    }
                }
                callback_(msg);
            }
        } else {
            const auto& t = outcome.tracks[0];
            if (t.stableId != "Unknown") {
                if (now - lastVerifiedMs_ > 800) {
                    lastVerifiedMs_ = now;
                    callback_("VERIFIED " + t.stableId + " " +
                        std::to_string(static_cast<int>(t.stableConfidence * 100)) + "%");
                }
            } else {
                if (now - lastUnknownMs_ > 1200) {
                    lastUnknownMs_ = now;
                    callback_("AUTH_FAIL Unknown");
                }
            }
        }
    }

    // 发布渲染帧
    if (!outcome.renderFrame.empty()) {
        std::lock_guard<std::mutex> lk(renderMu_);
        outcome.renderFrame.copyTo(renderFrame_);
        renderSeq_++;
    }
}

bool ResultPublisher::getRenderFrame(cv::Mat& out, uint64_t& seq) {
    std::lock_guard<std::mutex> lk(renderMu_);
    if (renderFrame_.empty()) return false;
    if (renderSeq_ == seq) return false;
    renderFrame_.copyTo(out);
    seq = renderSeq_;
    return true;
}

}  // namespace pipeline
