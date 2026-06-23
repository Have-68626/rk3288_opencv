#pragma once

#include <string>
#include <vector>

namespace cv {
class Mat;
}

namespace rk_win {

struct FaceMatch;  // defined in FaceRecognizer.h

class IRecognizer {
public:
    virtual ~IRecognizer() = default;

    virtual std::vector<FaceMatch> identify(const cv::Mat& bgr) = 0;
    virtual bool enrollFromFrame(const std::string& personId, const cv::Mat& bgr, int samplesToTake, int& samplesTakenOut) = 0;

    virtual bool saveDb() const = 0;
    virtual void clearDb() = 0;

    virtual double threshold() const = 0;
    virtual void setThreshold(double t) = 0;

    virtual std::vector<std::string> personIds() const = 0;
};

}  // namespace rk_win
