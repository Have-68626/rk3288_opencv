#include "rk_win/EventLogger.h"

#include "rk_win/StructuredLogger.h"

#include <filesystem>
#include <sstream>

namespace rk_win {

std::string EventLogger::escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

bool EventLogger::open(const std::filesystem::path& dir) {
    std::lock_guard<std::mutex> lock(mu_);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    path_ = dir / "event_log.jsonl";
    out_.open(path_, std::ios::out | std::ios::app);
    return out_.is_open();
}

void EventLogger::close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.is_open()) out_.close();
}

void EventLogger::append(const std::string& eventType, const std::string& message) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!out_.is_open()) return;
    out_ << "{"
         << "\"ts\":\"" << escapeJson(nowIso8601Local()) << "\","
         << "\"type\":\"" << escapeJson(eventType) << "\","
         << "\"msg\":\"" << escapeJson(message) << "\""
         << "}\n";
}

}  // namespace rk_win

