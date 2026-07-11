#include "rk_win/FaceDatabase.h"

#include <opencv2/core/persistence.hpp>

#include <algorithm>

namespace rk_win {

bool FaceDatabase::load(const std::filesystem::path& path) {
    if (path.empty()) return false;
    if (!std::filesystem::exists(path)) {
        std::lock_guard<std::mutex> lock(mu_);
        persons_.clear();
        return true;
    }

    cv::FileStorage fs(path.string(), cv::FileStorage::READ);
    if (!fs.isOpened()) return false;

    std::unordered_map<std::string, PersonEntry> tmp;
    cv::FileNode persons = fs["persons"];
    if (persons.type() == cv::FileNode::SEQ) {
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
            CV_Assert(meanMat.type() == CV_32F && meanMat.isContinuous() && e.mean.size() == (size_t)meanMat.total());
            std::copy(meanMat.ptr<float>(), meanMat.ptr<float>() + meanMat.total(), e.mean.begin());
            if (!e.id.empty() && !e.mean.empty()) {
                tmp.emplace(e.id, std::move(e));
            }
        }
    }

    std::lock_guard<std::mutex> lock(mu_);
    persons_ = std::move(tmp);
    return true;
}

bool FaceDatabase::save(const std::filesystem::path& path) const {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::unordered_map<std::string, PersonEntry> snap;
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap = persons_;
    }

    cv::FileStorage fs(path.string(), cv::FileStorage::WRITE);
    if (!fs.isOpened()) return false;

    fs << "version" << 1;
    fs << "persons" << "[";
    for (const auto& kv : snap) {
        const auto& e = kv.second;
        cv::Mat meanMat(1, static_cast<int>(e.mean.size()), CV_32F);
        CV_Assert(e.mean.size() * sizeof(float) == meanMat.total() * meanMat.elemSize());
        std::copy(e.mean.begin(), e.mean.end(), meanMat.ptr<float>());
        fs << "{";
        fs << "id" << e.id;
        fs << "count" << static_cast<int>(e.count);
        fs << "mean" << meanMat;
        fs << "}";
    }
    fs << "]";
    fs.release();
    return true;
}

void FaceDatabase::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    persons_.clear();
}

bool FaceDatabase::hasPerson(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    return persons_.find(id) != persons_.end();
}

std::vector<std::string> FaceDatabase::listPersonIds() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> ids;
    ids.reserve(persons_.size());
    for (const auto& kv : persons_) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool FaceDatabase::updateMean(const std::string& id, const std::vector<float>& embedding) {
    if (id.empty() || embedding.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
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

std::unordered_map<std::string, PersonEntry> FaceDatabase::persons() const {
    std::lock_guard<std::mutex> lock(mu_);
    return persons_;
}

}  // namespace rk_win

