#include "rk_win/FacesJson.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace rk_win {
namespace {

struct RectI {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

RectI clipRect(const RectI& r, int maxW, int maxH) {
    int x0 = std::max(0, r.x);
    int y0 = std::max(0, r.y);
    int x1 = std::min(maxW, r.x + r.w);
    int y1 = std::min(maxH, r.y + r.h);
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

RectI mapToPreview(const FaceMatch& f, const FacesSnapshot& s) {
    if (s.previewWidth <= 0 || s.previewHeight <= 0) return {};
    if (s.frameWidth <= 0 || s.frameHeight <= 0) return {};

    const double srcW = static_cast<double>(s.frameWidth);
    const double srcH = static_cast<double>(s.frameHeight);
    const double dstW = static_cast<double>(s.previewWidth);
    const double dstH = static_cast<double>(s.previewHeight);

    double scale = 1.0;
    if (s.previewScaleMode == 1) scale = std::min(dstW / srcW, dstH / srcH);
    else scale = std::max(dstW / srcW, dstH / srcH);

    const double scaledW = srcW * scale;
    const double scaledH = srcH * scale;
    const double offX = (dstW - scaledW) * 0.5;
    const double offY = (dstH - scaledH) * 0.5;

    RectI r;
    r.x = static_cast<int>(std::floor(offX + static_cast<double>(f.rect.x) * scale + 0.5));
    r.y = static_cast<int>(std::floor(offY + static_cast<double>(f.rect.y) * scale + 0.5));
    r.w = static_cast<int>(std::floor(static_cast<double>(f.rect.width) * scale + 0.5));
    r.h = static_cast<int>(std::floor(static_cast<double>(f.rect.height) * scale + 0.5));
    return clipRect(r, s.previewWidth, s.previewHeight);
}

std::string scaleModeText(int mode) {
    return (mode == 1) ? "letterbox" : "crop_fill";
}

}  // namespace

std::string buildFacesJson(const FacesSnapshot& s) {
    std::ostringstream oss;
    const std::uint64_t tsMs = s.timestamp100ns / 10000ULL;
    oss << "{";
    oss << "\"timestamp_ms\":" << tsMs << ",";
    oss << "\"frame\":{\"w\":" << s.frameWidth << ",\"h\":" << s.frameHeight << "},";
    oss << "\"preview\":{\"w\":" << s.previewWidth << ",\"h\":" << s.previewHeight << ",\"scale_mode\":\"" << scaleModeText(s.previewScaleMode) << "\"},";
    oss << "\"perf\":{\"infer_ms\":" << s.inferMs << ",\"drop_rate\":" << s.dropRate << ",\"stride\":" << s.stride << "},";
    oss << "\"faces\":[";
    for (size_t i = 0; i < s.faces.size(); i++) {
        const auto& f = s.faces[i];
        const RectI dr = mapToPreview(f, s);
        if (i) oss << ",";
        oss << "{";
        oss << "\"bbox\":{\"x\":" << f.rect.x << ",\"y\":" << f.rect.y << ",\"w\":" << f.rect.width << ",\"h\":" << f.rect.height << "},";
        oss << "\"display_bbox\":{\"x\":" << dr.x << ",\"y\":" << dr.y << ",\"w\":" << dr.w << ",\"h\":" << dr.h << "},";
        oss << "\"confidence\":" << f.confidence;
        oss << "}";
    }
    oss << "]";
    oss << "}";
    return oss.str();
}

}  // namespace rk_win

