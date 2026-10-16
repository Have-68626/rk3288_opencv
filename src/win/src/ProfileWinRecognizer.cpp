#include "rk_win/ProfileWinRecognizer.h"
#include "rk_win/FaceRecognizer.h"
#include "FaceAlign.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace rk_win {
namespace {
bool normalizeEmbedding(std::vector<float>& embedding, int dim) {
    if (embedding.size() != static_cast<std::size_t>(dim)) return false;
    double square = 0.0;
    for (float value : embedding) {
        if (!std::isfinite(value)) return false;
        square += static_cast<double>(value) * value;
    }
    if (square < 1e-12) return false;
    const double norm = std::sqrt(square);
    for (float& value : embedding) value = static_cast<float>(value / norm);
    return true;
}

const char* windowsProfileAbi() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64-v8a";
#elif defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#else
    return "x86";
#endif
}
}

ProfileWinRecognizer::ProfileWinRecognizer(rk_core::RealtimePipelineHooks hooks) : pipeline_(std::move(hooks)) {}

bool ProfileWinRecognizer::initialize(const std::filesystem::path& profileDir,
                                      const std::filesystem::path& databasePath, std::string& err) {
    ready_ = false;
    std::ifstream manifest(profileDir / "manifest.json", std::ios::binary);
    if (!manifest) { err = "profile manifest.json 不可读"; return false; }
    const std::string text{std::istreambuf_iterator<char>(manifest), {}};
    const auto reload = pipeline_.reload(text, profileDir.string(), windowsProfileAbi(), "");
    if (!reload.ok) { err = reload.reason + ": " + reload.evidence; return false; }
    const auto state = pipeline_.snapshot();
    const auto& space = state->profile.templateSpaceId;
    // 空空间沿用原路径；新空间绝不探测、搬迁或自动导入旧模板。
    if (!space.empty() && !rk_core::FaceRuntimeManifest::isValidTemplateSpaceId(space)) {
        err = "templateSpaceId 非法"; return false;
    }
    dbPath_ = space.empty() ? databasePath : databasePath.parent_path() / "face_spaces" / space / databasePath.filename();
    if (!db_.load(dbPath_)) { err = "profile 模板数据库加载失败"; dbPath_.clear(); return false; }
    for (const auto& entry : db_.persons()) {
        auto embedding = entry.second.mean;
        if (!normalizeEmbedding(embedding, state->profile.embeddingDim)) {
            err = "profile 模板数据库维度或向量非法"; dbPath_.clear(); return false;
        }
    }
    threshold_ = state->profile.identitySimilarity;
    ready_ = true;
    err.clear();
    return true;
}

std::shared_ptr<const rk_core::RealtimePipelineSnapshot> ProfileWinRecognizer::snapshot() const { return pipeline_.snapshot(); }

bool ProfileWinRecognizer::embedFace(const cv::Mat& bgr, const rk_core::RealtimeFaceBox& box,
                                      std::vector<float>& out) const {
    FaceDetection detection;
    detection.bbox = {box.x, box.y, box.width, box.height};
    detection.score = box.score;
    if (box.hasKeypoints) {
        std::array<cv::Point2f, 5> points;
        for (std::size_t i = 0; i < points.size(); ++i) points[i] = {box.keypoints[i * 2], box.keypoints[i * 2 + 1]};
        detection.keypoints5 = points;
    }
    const auto aligned = alignFaceForArcFace112(bgr, detection, FaceAlignOptions{});
    std::string err;
    if (aligned.alignedBgr.empty() || !pipeline_.embed(aligned.alignedBgr, out, err)) return false;
    return normalizeEmbedding(out, pipeline_.snapshot()->profile.embeddingDim);
}

std::vector<FaceMatch> ProfileWinRecognizer::identify(const cv::Mat& bgr) {
    std::vector<FaceMatch> matches;
    if (!ready_ || bgr.empty()) return matches;
    std::vector<rk_core::RealtimeFaceBox> boxes;
    std::string err;
    if (!pipeline_.detect(bgr, boxes, err)) return matches;
    const auto persons = db_.persons();
    for (const auto& box : boxes) {
        FaceMatch match;
        match.rect = cv::Rect(cvRound(box.x), cvRound(box.y), cvRound(box.width), cvRound(box.height)) & cv::Rect(0, 0, bgr.cols, bgr.rows);
        match.personId = "UNKNOWN";
        match.distance = 1.0;
        std::vector<float> embedding;
        if (embedFace(bgr, box, embedding)) {
            double best = -1.0;
            std::string bestId;
            for (const auto& entry : persons) {
                auto known = entry.second.mean;
                if (!normalizeEmbedding(known, static_cast<int>(embedding.size()))) continue;
                double similarity = 0.0;
                for (std::size_t i = 0; i < known.size(); ++i) similarity += static_cast<double>(known[i]) * embedding[i];
                if (similarity > best) { best = similarity; bestId = entry.first; }
            }
            match.distance = 1.0 - best;
            if (!bestId.empty() && best >= threshold_) {
                match.personId = bestId;
                match.accepted = true;
                match.confidence = std::clamp(best, 0.0, 1.0);
            }
        }
        matches.push_back(std::move(match));
    }
    return matches;
}

bool ProfileWinRecognizer::enrollFromFrame(const std::string& personId, const cv::Mat& bgr,
                                           int samplesToTake, int& samplesTakenOut) {
    samplesTakenOut = 0;
    if (!ready_ || personId.empty() || bgr.empty() || samplesToTake <= 0) return false;
    std::vector<rk_core::RealtimeFaceBox> boxes;
    std::string err;
    if (!pipeline_.detect(bgr, boxes, err) || boxes.empty()) return false;
    const auto largest = std::max_element(boxes.begin(), boxes.end(), [](const auto& a, const auto& b) {
        return a.width * a.height < b.width * b.height;
    });
    std::vector<float> embedding;
    if (!embedFace(bgr, *largest, embedding) || !db_.updateMean(personId, embedding)) return false;
    samplesTakenOut = 1;
    return true;
}

bool ProfileWinRecognizer::saveDb() const { return ready_ && !dbPath_.empty() && db_.save(dbPath_); }
void ProfileWinRecognizer::clearDb() {
    if (!ready_) return;
    db_.clear();
    saveDb();
}
double ProfileWinRecognizer::threshold() const { return threshold_; }
void ProfileWinRecognizer::setThreshold(double t) { if (std::isfinite(t) && t > 0.0 && t <= 1.0) threshold_ = t; }
std::vector<std::string> ProfileWinRecognizer::personIds() const { return db_.listPersonIds(); }

} // namespace rk_win
