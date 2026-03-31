#include "rk_win/LbphEmbedder.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace rk_win {
namespace {

inline unsigned char lbpAt(const cv::Mat& g, int x, int y) {
    const unsigned char c = g.at<unsigned char>(y, x);
    unsigned char code = 0;
    code |= (g.at<unsigned char>(y - 1, x - 1) >= c) ? 128 : 0;
    code |= (g.at<unsigned char>(y - 1, x) >= c) ? 64 : 0;
    code |= (g.at<unsigned char>(y - 1, x + 1) >= c) ? 32 : 0;
    code |= (g.at<unsigned char>(y, x + 1) >= c) ? 16 : 0;
    code |= (g.at<unsigned char>(y + 1, x + 1) >= c) ? 8 : 0;
    code |= (g.at<unsigned char>(y + 1, x) >= c) ? 4 : 0;
    code |= (g.at<unsigned char>(y + 1, x - 1) >= c) ? 2 : 0;
    code |= (g.at<unsigned char>(y, x - 1) >= c) ? 1 : 0;
    return code;
}

}  // namespace

std::vector<float> LbphEmbedder::embedFaceGray(const cv::Mat& faceGray) const {
    std::vector<float> out;
    if (faceGray.empty()) return out;

    cv::Mat g;
    if (faceGray.type() == CV_8UC1) {
        g = faceGray;
    } else {
        cv::cvtColor(faceGray, g, cv::COLOR_BGR2GRAY);
    }

    cv::Mat resized;
    cv::resize(g, resized, cv::Size(resizeWidth, resizeHeight), 0, 0, cv::INTER_LINEAR);
    cv::equalizeHist(resized, resized);

    const int w = resized.cols;
    const int h = resized.rows;
    const int cellW = std::max(1, w / gridX);
    const int cellH = std::max(1, h / gridY);

    const int dim = gridX * gridY * 256;
    out.assign(dim, 0.0f);

    for (int y = 1; y < h - 1; y++) {
        const int cy = std::min(gridY - 1, y / cellH);
        for (int x = 1; x < w - 1; x++) {
            const int cx = std::min(gridX - 1, x / cellW);
            const int cellIndex = (cy * gridX + cx) * 256;
            const unsigned char code = lbpAt(resized, x, y);
            out[cellIndex + static_cast<int>(code)] += 1.0f;
        }
    }

    for (int cy = 0; cy < gridY; cy++) {
        for (int cx = 0; cx < gridX; cx++) {
            float sum = 0.0f;
            const int cellIndex = (cy * gridX + cx) * 256;
            for (int i = 0; i < 256; i++) sum += out[cellIndex + i];
            if (sum <= 0.0f) continue;
            for (int i = 0; i < 256; i++) out[cellIndex + i] /= sum;
        }
    }
    return out;
}

double LbphEmbedder::chiSquareDistance(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    double d = 0.0;
    for (size_t i = 0; i < a.size(); i++) {
        const double ai = static_cast<double>(a[i]);
        const double bi = static_cast<double>(b[i]);
        const double num = (ai - bi) * (ai - bi);
        const double den = ai + bi + 1e-12;
        d += num / den;
    }
    return 0.5 * d;
}

}  // namespace rk_win

