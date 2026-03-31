#include "rk_win/OverlayRenderer.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace rk_win {
namespace {

void drawRoundedRect(cv::Mat& img, const cv::Rect& r0, int radius, int thickness, const cv::Scalar& bgr) {
    if (img.empty()) return;
    cv::Rect r = r0 & cv::Rect(0, 0, img.cols, img.rows);
    if (r.width <= 0 || r.height <= 0) return;

    const int rad = std::max(0, std::min({radius, r.width / 2, r.height / 2}));
    const int x0 = r.x;
    const int y0 = r.y;
    const int x1 = r.x + r.width;
    const int y1 = r.y + r.height;

    const int lt = cv::LINE_AA;
    cv::line(img, cv::Point(x0 + rad, y0), cv::Point(x1 - rad, y0), bgr, thickness, lt);
    cv::line(img, cv::Point(x0 + rad, y1), cv::Point(x1 - rad, y1), bgr, thickness, lt);
    cv::line(img, cv::Point(x0, y0 + rad), cv::Point(x0, y1 - rad), bgr, thickness, lt);
    cv::line(img, cv::Point(x1, y0 + rad), cv::Point(x1, y1 - rad), bgr, thickness, lt);

    if (rad > 0) {
        cv::ellipse(img, cv::Point(x0 + rad, y0 + rad), cv::Size(rad, rad), 0, 180, 270, bgr, thickness, lt);
        cv::ellipse(img, cv::Point(x1 - rad, y0 + rad), cv::Size(rad, rad), 0, 270, 360, bgr, thickness, lt);
        cv::ellipse(img, cv::Point(x0 + rad, y1 - rad), cv::Size(rad, rad), 0, 90, 180, bgr, thickness, lt);
        cv::ellipse(img, cv::Point(x1 - rad, y1 - rad), cv::Size(rad, rad), 0, 0, 90, bgr, thickness, lt);
    }
}

std::string confidenceToPercentText(double conf01) {
    double c = conf01;
    if (!std::isfinite(c)) c = 0.0;
    c = std::clamp(c, 0.0, 1.0);
    const int pct = static_cast<int>(std::round(c * 100.0));
    return std::to_string(pct) + "%";
}

}  // namespace

void drawFacesOverlay(cv::Mat& bgr, const std::vector<FaceMatch>& faces) {
    if (bgr.empty()) return;
    if (faces.empty()) return;

    cv::Mat overlay = bgr.clone();
    const cv::Scalar white(255, 255, 255);
    const int thickness = 2;
    const int radius = 5;

    for (const auto& f : faces) {
        drawRoundedRect(overlay, f.rect, radius, thickness, white);
        const std::string text = confidenceToPercentText(f.confidence);
        const int x = std::max(0, f.rect.x + radius);
        const int y = std::max(0, f.rect.y + radius + 18);
        cv::putText(overlay, text, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX, 0.6, white, 2, cv::LINE_AA);
    }

    cv::addWeighted(overlay, 0.70, bgr, 0.30, 0.0, bgr);
}

}  // namespace rk_win

