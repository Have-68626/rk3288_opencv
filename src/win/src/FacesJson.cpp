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
    /*
     * [Performance Optimization - string formatting]
     * Why: Replace std::ostringstream with std::string concatenation to avoid virtual calls and locale overhead per frame log.
     * Impact: Lower CPU usage during JSON logging in the processing loop.
     * Rollback: Revert back to using std::ostringstream.
     */
    std::string jsonStr;
    jsonStr.reserve(256 + s.faces.size() * 128);

    const std::uint64_t tsMs = s.timestamp100ns / 10000ULL;

    jsonStr += "{\"timestamp_ms\":";
    jsonStr += std::to_string(tsMs);
    jsonStr += ",\"frame\":{\"w\":";
    jsonStr += std::to_string(s.frameWidth);
    jsonStr += ",\"h\":";
    jsonStr += std::to_string(s.frameHeight);
    jsonStr += "},\"preview\":{\"w\":";
    jsonStr += std::to_string(s.previewWidth);
    jsonStr += ",\"h\":";
    jsonStr += std::to_string(s.previewHeight);
    jsonStr += ",\"scale_mode\":\"";
    jsonStr += scaleModeText(s.previewScaleMode);
    jsonStr += "\"},\"perf\":{\"infer_ms\":";
    char buf[512];
    if (std::isfinite(s.inferMs)) {
        std::snprintf(buf, sizeof(buf), "%g", s.inferMs);
        jsonStr += buf;
    } else {
        jsonStr += "null";
    }
    jsonStr += ",\"drop_rate\":";
    if (std::isfinite(s.dropRate)) {
        std::snprintf(buf, sizeof(buf), "%g", s.dropRate);
        jsonStr += buf;
    } else {
        jsonStr += "null";
    }
    jsonStr += ",\"stride\":";
    jsonStr += std::to_string(s.stride);
    jsonStr += "},\"faces\":[";

    for (size_t i = 0; i < s.faces.size(); i++) {
        const auto& f = s.faces[i];
        const RectI dr = mapToPreview(f, s);
        if (i) jsonStr += ",";
        jsonStr += "{\"bbox\":{\"x\":";
        jsonStr += std::to_string(f.rect.x);
        jsonStr += ",\"y\":";
        jsonStr += std::to_string(f.rect.y);
        jsonStr += ",\"w\":";
        jsonStr += std::to_string(f.rect.width);
        jsonStr += ",\"h\":";
        jsonStr += std::to_string(f.rect.height);
        jsonStr += "},\"display_bbox\":{\"x\":";
        jsonStr += std::to_string(dr.x);
        jsonStr += ",\"y\":";
        jsonStr += std::to_string(dr.y);
        jsonStr += ",\"w\":";
        jsonStr += std::to_string(dr.w);
        jsonStr += ",\"h\":";
        jsonStr += std::to_string(dr.h);
        jsonStr += "},\"confidence\":";
        if (std::isfinite(f.confidence)) {
            std::snprintf(buf, sizeof(buf), "%g", f.confidence);
            jsonStr += buf;
        } else {
            jsonStr += "null";
        }
        jsonStr += "}";
    }
    jsonStr += "]}";
    return jsonStr;
}

}  // namespace rk_win
