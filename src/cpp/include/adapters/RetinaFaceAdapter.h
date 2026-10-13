#pragma once

#include "FaceDetector.h"

#include <opencv2/dnn.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rk_core {

class RetinaFaceAdapter : public FaceDetector {
public:
    RetinaFaceAdapter();
    ~RetinaFaceAdapter() override;

    bool load(const std::string& modelPath, std::string& err) override;
    FaceDetections detect(const cv::Mat& bgr, std::string& err) override;
    const char* name() const override;

private:
    struct AnchorCfg {
        int stride;
        std::vector<int> baseSize;
        std::vector<float> ratios;
        std::vector<float> scales;
    };

    void generateAnchors(int stride, const std::vector<int>& baseSize,
                         const std::vector<float>& ratios, const std::vector<float>& scales,
                         int featW, int featH, std::vector<std::vector<float>>& anchors);
    float intersectArea(float ax1, float ay1, float ax2, float ay2,
                        float bx1, float by1, float bx2, float by2);
    void nms(std::vector<FaceDetection>& dets, float threshold);

    /** Cache key: (stride, featW, featH) → precomputed anchors */
    struct AnchorCacheKey {
        int stride, featW, featH;
        bool operator==(const AnchorCacheKey& o) const { return stride == o.stride && featW == o.featW && featH == o.featH; }
    };
    struct AnchorCacheKeyHash {
        std::size_t operator()(const AnchorCacheKey& k) const {
            return static_cast<std::size_t>(k.stride) ^ (static_cast<std::size_t>(k.featW) << 10) ^ (static_cast<std::size_t>(k.featH) << 20);
        }
    };
    mutable std::mutex mu_;
    std::unordered_map<AnchorCacheKey, std::vector<std::vector<float>>, AnchorCacheKeyHash> anchorCache_;
    cv::dnn::Net net_;
    int inputW_ = 640;
    int inputH_ = 640;
    float confThreshold_ = 0.5f;
    float nmsThreshold_ = 0.4f;
    int topK_ = 5000;
    bool loaded_ = false;
    std::string currentName_;
    std::vector<AnchorCfg> anchorCfgs_;
    std::vector<std::string> outputNames_;
};

} // namespace rk_core
