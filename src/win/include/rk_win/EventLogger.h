#pragma once

#include "Compat.h"
#include <fstream>
#include <mutex>
#include <string>

namespace rk_win {

class EventLogger {
public:
    bool open(const std::filesystem::path& dir);
    void close();

    void append(const std::string& eventType, const std::string& message);

private:
    static std::string escapeJson(const std::string& s);

    std::mutex mu_;
    std::filesystem::path path_;
    std::ofstream out_;
};

}  // namespace rk_win

