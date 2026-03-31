#include "rk_win/FaceDatabase.h"

#include <opencv2/core/persistence.hpp>

#include <algorithm>

namespace rk_win {

bool FaceDatabase::load(const std::filesystem::path& path) {
    persons_.clear();
    if (path.empty()) return false;
    if (!std::filesystem::exists(path)) return true;

    cv::FileStorage fs(path.string(), cv::FileStorage::READ);
    if (!fs.isOpened()) return false;

    cv::FileNode persons = fs["persons"];
    if (persons.type() != cv::FileNode::SEQ) return true;

    for (auto it = persons.begin(); it != persons.end(); ++it) {
        PersonEntry e;
        (*it)["id"] >> e.id;
        int count = 0;
        (*it)["count"] >> count;
        e.count = count;
        cv::Mat meanMat;
        (*it)["mean"] >> meanMat;
        meanMat = meanMat.reshape(1, 1);
        e.mean.resize(static_cast<size_t>(meanMat.cols));
        for (int i = 0; i < meanMat.cols; i++) {
            e.mean[static_cast<size_t>(i)] = meanMat.at<float>(0, i);
        }
        if (!e.id.empty() && !e.mean.empty()) {
            persons_.emplace(e.id, std::move(e));
        }
    }

    return true;
}

bool FaceDatabase::save(const std::filesystem::path& path) const {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    cv::FileStorage fs(path.string(), cv::FileStorage::WRITE);
    if (!fs.isOpened()) return false;

    fs << "version" << 1;
    fs << "persons" << "[";
    for (const auto& kv : persons_) {
        const auto& e = kv.second;
        cv::Mat meanMat(1, static_cast<int>(e.mean.size()), CV_32F);
        for (int i = 0; i < meanMat.cols; i++) {
            meanMat.at<float>(0, i) = e.mean[static_cast<size_t>(i)];
        }
        fs << "{";
        fs << "id" << e.id;
        fs << "count" << e.count;
        fs << "mean" << meanMat;
        fs << "}";
    }
    fs << "]";
    fs.release();
    return true;
}

void FaceDatabase::clear() {
    persons_.clear();
}

bool FaceDatabase::hasPerson(const std::string& id) const {
    return persons_.find(id) != persons_.end();
}

std::vector<std::string> FaceDatabase::listPersonIds() const {
    std::vector<std::string> ids;
    ids.reserve(persons_.size());
    for (const auto& kv : persons_) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool FaceDatabase::updateMean(const std::string& id, const std::vector<float>& embedding) {
    if (id.empty() || embedding.empty()) return false;
    auto& e = persons_[id];
    if (e.id.empty()) e.id = id;

    if (e.mean.empty()) {
        e.mean = embedding;
        e.count = 1;
        return true;
    }
    if (e.mean.size() != embedding.size()) return false;

    const double prev = static_cast<double>(e.count);
    const double next = prev + 1.0;
    for (size_t i = 0; i < e.mean.size(); i++) {
        const double v = (static_cast<double>(e.mean[i]) * prev + static_cast<double>(embedding[i])) / next;
        e.mean[i] = static_cast<float>(v);
    }
    e.count++;
    return true;
}

const std::unordered_map<std::string, PersonEntry>& FaceDatabase::persons() const {
    return persons_;
}

}  // namespace rk_win

