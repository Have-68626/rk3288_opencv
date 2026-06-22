#include "rk_win/ArcFaceWinRecognizer.h"
#include "rk_win/FaceRecognizer.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace rk_win {

bool ArcFaceWinRecognizer::initialize(const std::string& cascadePath, const std::filesystem::path& dbPath,
                                       const std::string& arcFaceModelPath, int minFaceSizePx, double identifyThreshold) {
    if (!detector_.loadCascade(cascadePath)) return false;

    embedderCfg_.backend = ArcFaceEmbedderConfig::BackendType::OpenCvDnn;
    embedderCfg_.opencvModel = arcFaceModelPath;
    embedderCfg_.opencvFramework = "onnx";
    embedderCfg_.opencvOutput = "output";
    embedderCfg_.opencvInput = "data";

    std::string err;
    if (!embedder_.initialize(embedderCfg_, &err)) {
        std::cerr << "[ArcFaceWinRecognizer] initialize failed: " << err << std::endl;
        return false;
    }

    dbPath_ = dbPath;
    minFaceSizePx_ = minFaceSizePx;
    identifyThreshold_ = identifyThreshold;
    return db_.load(dbPath_);
}

cv::Rect ArcFaceWinRecognizer::pickLargest(const std::vector<cv::Rect>& faces) {
    if (faces.empty()) return {};
    auto best = faces[0];
    for (const auto& r : faces) {
        if (r.area() > best.area()) best = r;
    }
    return best;
}

cv::Mat ArcFaceWinRecognizer::cropAndNormalizeFaceBgr(const cv::Mat& bgr, const cv::Rect& faceRect) {
    cv::Rect r = faceRect & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (r.empty()) return {};

    const int padX = static_cast<int>(r.width * 0.15);
    const int padY = static_cast<int>(r.height * 0.15);
    r.x = std::max(0, r.x - padX);
    r.y = std::max(0, r.y - padY);
    r.width = std::min(bgr.cols - r.x, r.width + padX * 2);
    r.height = std::min(bgr.rows - r.y, r.height + padY * 2);

    cv::Mat face;
    cv::resize(bgr(r), face, cv::Size(112, 112), 0, 0, cv::INTER_LINEAR);
    return face;
}

void ArcFaceWinRecognizer::l2NormalizeInplace(std::vector<float>& v) {
    float sqSum = 0.0f;
    for (float x : v) sqSum += x * x;
    if (sqSum < 1e-12f) return;
    float inv = 1.0f / std::sqrt(sqSum);
    for (float& x : v) x *= inv;
}

float ArcFaceWinRecognizer::cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 0.0f;
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float denom = std::sqrt(na) * std::sqrt(nb);
    return (denom < 1e-12f) ? 0.0f : dot / denom;
}

std::vector<FaceMatch> ArcFaceWinRecognizer::identify(const cv::Mat& bgr) {
    std::vector<FaceMatch> out;
    if (bgr.empty()) return out;

    const auto faces = detector_.detect(bgr, minFaceSizePx_);
    out.reserve(faces.size());

    for (const auto& fr : faces) {
        FaceMatch m;
        m.rect = fr;

        cv::Mat faceBgr = cropAndNormalizeFaceBgr(bgr, fr);
        if (faceBgr.empty()) {
            m.personId = "UNKNOWN";
            m.distance = 1.0;
            m.confidence = 0.0;
            m.accepted = false;
            out.push_back(std::move(m));
            continue;
        }

        std::string err;
        auto optEmb = embedder_.embedAlignedFaceBgr(faceBgr, &err);
        if (!optEmb) {
            m.personId = "UNKNOWN";
            m.distance = 1.0;
            m.confidence = 0.0;
            m.accepted = false;
            out.push_back(std::move(m));
            continue;
        }

        auto emb = std::move(optEmb->values);
        l2NormalizeInplace(emb);

        float bestSim = -1.0f;
        std::string bestId;

        for (const auto& kv : db_.persons()) {
            auto dbEmb = kv.second.mean;
            l2NormalizeInplace(dbEmb);
            float sim = cosineSimilarity(emb, dbEmb);
            if (sim > bestSim) {
                bestSim = sim;
                bestId = kv.first;
            }
        }

        m.distance = 1.0f - bestSim;  // cosine distance
        if (bestSim >= identifyThreshold_) {
            m.personId = bestId;
            m.accepted = true;
            m.confidence = std::clamp(bestSim, 0.0f, 1.0f);
        } else {
            m.personId = "UNKNOWN";
            m.accepted = false;
            m.confidence = 0.0;
        }

        out.push_back(std::move(m));
    }

    return out;
}

bool ArcFaceWinRecognizer::enrollFromFrame(const std::string& personId, const cv::Mat& bgr, int samplesToTake, int& samplesTakenOut) {
    samplesTakenOut = 0;
    if (personId.empty() || bgr.empty()) return false;
    if (samplesToTake <= 0) return false;

    const auto faces = detector_.detect(bgr, minFaceSizePx_);
    if (faces.empty()) return false;
    const cv::Rect face = pickLargest(faces);

    cv::Mat faceBgr = cropAndNormalizeFaceBgr(bgr, face);
    if (faceBgr.empty()) return false;

    std::string err;
    auto optEmb = embedder_.embedAlignedFaceBgr(faceBgr, &err);
    if (!optEmb) return false;

    auto emb = std::move(optEmb->values);
    l2NormalizeInplace(emb);

    if (!db_.updateMean(personId, emb)) return false;
    samplesTakenOut = 1;
    return true;
}

bool ArcFaceWinRecognizer::saveDb() const {
    return db_.save(dbPath_);
}

void ArcFaceWinRecognizer::clearDb() {
    db_.clear();
}

double ArcFaceWinRecognizer::threshold() const {
    return identifyThreshold_;
}

void ArcFaceWinRecognizer::setThreshold(double t) {
    identifyThreshold_ = t;
}

std::vector<std::string> ArcFaceWinRecognizer::personIds() const {
    return db_.listPersonIds();
}

}  // namespace rk_win
