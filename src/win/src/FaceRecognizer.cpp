#include "rk_win/FaceRecognizer.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <limits>

namespace rk_win {

bool FaceRecognizer::initialize(const std::string& cascadePath, const std::filesystem::path& dbPath, int minFaceSizePx, double identifyThreshold) {
    if (!detector_.loadCascade(cascadePath)) return false;
    dbPath_ = dbPath;
    minFaceSizePx_ = minFaceSizePx;
    identifyThreshold_ = identifyThreshold;
    return db_.load(dbPath_);
}

cv::Rect FaceRecognizer::pickLargest(const std::vector<cv::Rect>& faces) {
    if (faces.empty()) return {};
    auto best = faces[0];
    for (const auto& r : faces) {
        if (r.area() > best.area()) best = r;
    }
    return best;
}

cv::Mat FaceRecognizer::cropAndNormalizeFaceGray(const cv::Mat& bgr, const cv::Rect& faceRect, int w, int h) {
    cv::Rect r = faceRect & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (r.empty()) return {};

    const int padX = static_cast<int>(r.width * 0.10);
    const int padY = static_cast<int>(r.height * 0.10);
    r.x = std::max(0, r.x - padX);
    r.y = std::max(0, r.y - padY);
    r.width = std::min(bgr.cols - r.x, r.width + padX * 2);
    r.height = std::min(bgr.rows - r.y, r.height + padY * 2);

    cv::Mat crop = bgr(r).clone();
    cv::Mat gray;
    cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY);
    cv::resize(gray, gray, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
    cv::equalizeHist(gray, gray);
    return gray;
}

std::vector<FaceMatch> FaceRecognizer::identify(const cv::Mat& bgr) {
    std::vector<FaceMatch> out;
    if (bgr.empty()) return out;

    const auto faces = detector_.detect(bgr, minFaceSizePx_);
    out.reserve(faces.size());

    for (const auto& fr : faces) {
        FaceMatch m;
        m.rect = fr;

        cv::Mat faceGray = cropAndNormalizeFaceGray(bgr, fr, embedder_.resizeWidth, embedder_.resizeHeight);
        if (faceGray.empty()) {
            m.personId = "UNKNOWN";
            m.distance = 1e9;
            m.confidence = 0.0;
            m.accepted = false;
            out.push_back(std::move(m));
            continue;
        }

        const auto emb = embedder_.embedFaceGray(faceGray);
        double bestDist = std::numeric_limits<double>::infinity();
        std::string bestId;

        for (const auto& kv : db_.persons()) {
            const auto& pid = kv.first;
            const auto& mean = kv.second.mean;
            const double d = LbphEmbedder::chiSquareDistance(emb, mean);
            if (d < bestDist) {
                bestDist = d;
                bestId = pid;
            }
        }

        m.distance = std::isfinite(bestDist) ? bestDist : 1e9;
        if (!bestId.empty() && m.distance <= identifyThreshold_) {
            m.personId = bestId;
            m.accepted = true;
            const double c = 1.0 - (m.distance / std::max(identifyThreshold_, 1e-9));
            m.confidence = std::clamp(c, 0.0, 1.0);
        } else {
            m.personId = "UNKNOWN";
            m.accepted = false;
            m.confidence = 0.0;
        }

        out.push_back(std::move(m));
    }

    return out;
}

bool FaceRecognizer::enrollFromFrame(const std::string& personId, const cv::Mat& bgr, int samplesToTake, int& samplesTakenOut) {
    samplesTakenOut = 0;
    if (personId.empty() || bgr.empty()) return false;
    if (samplesToTake <= 0) return false;

    const auto faces = detector_.detect(bgr, minFaceSizePx_);
    if (faces.empty()) return false;
    const cv::Rect face = pickLargest(faces);

    cv::Mat faceGray = cropAndNormalizeFaceGray(bgr, face, embedder_.resizeWidth, embedder_.resizeHeight);
    if (faceGray.empty()) return false;
    const auto emb = embedder_.embedFaceGray(faceGray);
    if (emb.empty()) return false;

    if (!db_.updateMean(personId, emb)) return false;
    samplesTakenOut = 1;
    return true;
}

bool FaceRecognizer::saveDb() const {
    return db_.save(dbPath_);
}

void FaceRecognizer::clearDb() {
    db_.clear();
}

double FaceRecognizer::threshold() const {
    return identifyThreshold_;
}

void FaceRecognizer::setThreshold(double t) {
    identifyThreshold_ = t;
}

std::vector<std::string> FaceRecognizer::personIds() const {
    return db_.listPersonIds();
}

}  // namespace rk_win

