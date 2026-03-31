#pragma once

#include "Compat.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace rk_win {

struct PersonEntry {
    std::string id;
    std::vector<float> mean;
    int count = 0;
};

class FaceDatabase {
public:
    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    void clear();
    bool hasPerson(const std::string& id) const;
    std::vector<std::string> listPersonIds() const;

    bool updateMean(const std::string& id, const std::vector<float>& embedding);
    const std::unordered_map<std::string, PersonEntry>& persons() const;

private:
    std::unordered_map<std::string, PersonEntry> persons_;
};

}  // namespace rk_win

