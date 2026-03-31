#include "rk_win/LbphEmbedder.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <iostream>

namespace {

bool approxZero(double v) {
    return std::fabs(v) < 1e-9;
}

}  // namespace

bool test_lbph_embedder_dim_and_distance() {
    rk_win::LbphEmbedder e;

    cv::Mat img1(200, 200, CV_8UC1);
    cv::randu(img1, 0, 255);

    cv::Mat img2 = img1.clone();
    cv::GaussianBlur(img2, img2, cv::Size(7, 7), 1.5);

    const auto emb1 = e.embedFaceGray(img1);
    const auto emb2 = e.embedFaceGray(img2);

    if (emb1.empty() || emb2.empty()) return false;
    if (emb1.size() != emb2.size()) return false;
    if (emb1.size() != static_cast<size_t>(e.gridX * e.gridY * 256)) return false;

    const double d11 = rk_win::LbphEmbedder::chiSquareDistance(emb1, emb1);
    const double d12 = rk_win::LbphEmbedder::chiSquareDistance(emb1, emb2);
    if (!approxZero(d11)) return false;
    if (!(d12 >= 0.0)) return false;

    return true;
}

