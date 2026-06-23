#pragma once

#include "FaceDatabase.h"
#include "FaceDetector.h"
#include "LbphEmbedder.h"

#include "Compat.h"
#include <string>
#include <vector>

#ifndef RK_WIN_HAS_OPENCV
#if __has_include(<opencv2/core.hpp>)
#define RK_WIN_HAS_OPENCV 1
#else
#define RK_WIN_HAS_OPENCV 0
#endif
#endif

#if RK_WIN_HAS_OPENCV
#include <opencv2/core.hpp>
#else
namespace cv {
struct Mat;
#ifndef RK_WIN_STUB_CV_RECT_DEFINED
#define RK_WIN_STUB_CV_RECT_DEFINED 1
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};
#endif
}
#endif

namespace rk_win {

struct FaceMatch {
    cv::Rect rect;
    std::string personId;
    double distance = 0.0;
    double confidence = 0.0;
    bool accepted = false;
};

class FaceRecognizer {
public:
    bool initialize(const std::string& cascadePath, const std::filesystem::path& dbPath, int minFaceSizePx, double identifyThreshold);

    std::vector<FaceMatch> identify(const cv::Mat& bgr);
    bool enrollFromFrame(const std::string& personId, const cv::Mat& bgr, int samplesToTake, int& samplesTakenOut);

    bool saveDb() const;
    void clearDb();

    double threshold() const;
    void setThreshold(double t);

    std::vector<std::string> personIds() const;

private:
    static cv::Rect pickLargest(const std::vector<cv::Rect>& faces);
    static cv::Mat cropAndNormalizeFaceGray(const cv::Mat& bgr, const cv::Rect& faceRect, int w, int h);

    FaceDetector detector_;
    LbphEmbedder embedder_;
    FaceDatabase db_;
    std::filesystem::path dbPath_;
    int minFaceSizePx_ = 60;
    double identifyThreshold_ = 55.0;
};

}  // namespace rk_win

