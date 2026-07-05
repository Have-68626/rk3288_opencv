#pragma once

#include "FaceDatabase.h"
#include "FaceDetector.h"
#include "IRecognizer.h"

#include "Compat.h"
#include <ArcFaceEmbedder.h>

#include <string>
#include <vector>

namespace rk_win {

using rk_core::ArcFaceEmbedder;
using rk_core::ArcFaceEmbedderConfig;

class ArcFaceWinRecognizer : public IRecognizer {
public:
    bool initialize(const std::string& cascadePath, const std::filesystem::path& dbPath,
                    const std::string& arcFaceModelPath, int minFaceSizePx, double identifyThreshold);

    std::vector<FaceMatch> identify(const cv::Mat& bgr) override;
    bool enrollFromFrame(const std::string& personId, const cv::Mat& bgr, int samplesToTake, int& samplesTakenOut) override;

    bool saveDb() const override;
    void clearDb() override;
    double threshold() const override;
    void setThreshold(double t) override;
    std::vector<std::string> personIds() const override;

private:
    static cv::Rect pickLargest(const std::vector<cv::Rect>& faces);
    static cv::Mat cropAndNormalizeFaceBgr(const cv::Mat& bgr, const cv::Rect& faceRect);

    static float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);
    static void l2NormalizeInplace(std::vector<float>& v);

    FaceDetector detector_;
    ArcFaceEmbedder embedder_;
    ArcFaceEmbedderConfig embedderCfg_;
    FaceDatabase db_;
    std::filesystem::path dbPath_;
    int minFaceSizePx_ = 60;
    double identifyThreshold_ = 0.45;  // 余弦相似度阈值（小于此值拒绝）
};

}  // namespace rk_win
