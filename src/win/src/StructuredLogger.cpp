#include "rk_win/StructuredLogger.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace rk_win {
namespace {

std::uint64_t fileSizeOr0(const std::filesystem::path& p) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(sz);
}

void renameBestEffort(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::remove(to, ec);
    ec.clear();
    std::filesystem::rename(from, to, ec);
}

}  // namespace

std::string nowIso8601Local() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto tt = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << "." << std::setw(3) << std::setfill('0') << ms;
    return oss.str();
}

std::string toString(ErrorCategory c) {
    switch (c) {
        case ErrorCategory::None:
            return "none";
        case ErrorCategory::PrivacyDenied:
            return "privacy_denied";
        case ErrorCategory::DeviceNotFound:
            return "device_not_found";
        case ErrorCategory::DeviceBusy:
            return "device_busy";
        case ErrorCategory::FormatNotSupported:
            return "format_not_supported";
        case ErrorCategory::BackendFailure:
            return "backend_failure";
        case ErrorCategory::Unknown:
        default:
            return "unknown";
    }
}

bool StructuredLogger::open(const std::filesystem::path& dir, std::uint64_t maxBytes, int maxRollFiles) {
    std::lock_guard<std::mutex> lock(mu_);
    dir_ = dir;
    maxBytes_ = maxBytes;
    maxRollFiles_ = maxRollFiles;

    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);

    csvPath_ = dir_ / "recognition.csv";
    jsonlPath_ = dir_ / "recognition.jsonl";

    csv_.open(csvPath_, std::ios::out | std::ios::app);
    jsonl_.open(jsonlPath_, std::ios::out | std::ios::app);
    headerWritten_ = (fileSizeOr0(csvPath_) > 0);
    return csv_.is_open() && jsonl_.is_open();
}

void StructuredLogger::close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (csv_.is_open()) csv_.close();
    if (jsonl_.is_open()) jsonl_.close();
}

std::string StructuredLogger::escapeCsv(const std::string& s) {
    bool needQuote = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needQuote = true;
            break;
        }
    }
    if (!needQuote) return s;
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string StructuredLogger::escapeJson(const std::string& s) {
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

void StructuredLogger::rollIfNeededLocked(const std::filesystem::path& path) {
    if (maxBytes_ == 0 || maxRollFiles_ <= 0) return;
    const std::uint64_t sz = fileSizeOr0(path);
    if (sz <= maxBytes_) return;

    for (int i = maxRollFiles_ - 1; i >= 1; --i) {
        const std::filesystem::path from = path;
        const std::filesystem::path older = from.string() + "." + std::to_string(i);
        const std::filesystem::path newer = from.string() + "." + std::to_string(i + 1);
        if (std::filesystem::exists(older)) {
            renameBestEffort(older, newer);
        }
    }
    const std::filesystem::path first = path.string() + ".1";
    renameBestEffort(path, first);
}

void StructuredLogger::append(const FrameLogEntry& e) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!csv_.is_open() || !jsonl_.is_open()) return;

    rollIfNeededLocked(csvPath_);
    rollIfNeededLocked(jsonlPath_);

    if (!headerWritten_) {
        csv_ << "ts,camera_name,camera_id,frame_index,frame_w,frame_h,fps,face_count,faces_json,error_category,error_code,error_message\n";
        headerWritten_ = true;
    }

    std::ostringstream facesJson;
    facesJson << "[";
    for (size_t i = 0; i < e.faces.size(); i++) {
        if (i) facesJson << ",";
        const auto& f = e.faces[i];
        facesJson << "{"
                  << "\"x\":" << f.x << ","
                  << "\"y\":" << f.y << ","
                  << "\"w\":" << f.w << ","
                  << "\"h\":" << f.h << ","
                  << "\"person_id\":\"" << escapeJson(f.personId) << "\","
                  << "\"distance\":" << f.distance << ","
                  << "\"confidence\":" << f.confidence
                  << "}";
    }
    facesJson << "]";

    csv_ << escapeCsv(e.tsIso8601) << "," << escapeCsv(e.cameraName) << "," << escapeCsv(e.cameraId) << ","
         << e.frameIndex << "," << e.frameWidth << "," << e.frameHeight << "," << e.fps << ","
         << static_cast<int>(e.faces.size()) << "," << escapeCsv(facesJson.str()) << ","
         << escapeCsv(toString(e.errorCategory)) << "," << escapeCsv(e.errorCode) << "," << escapeCsv(e.errorMessage) << "\n";

    jsonl_ << "{"
           << "\"ts\":\"" << escapeJson(e.tsIso8601) << "\","
           << "\"camera_name\":\"" << escapeJson(e.cameraName) << "\","
           << "\"camera_id\":\"" << escapeJson(e.cameraId) << "\","
           << "\"frame_index\":" << e.frameIndex << ","
           << "\"frame_w\":" << e.frameWidth << ","
           << "\"frame_h\":" << e.frameHeight << ","
           << "\"fps\":" << e.fps << ","
           << "\"face_count\":" << static_cast<int>(e.faces.size()) << ","
           << "\"faces\":" << facesJson.str() << ","
           << "\"error_category\":\"" << escapeJson(toString(e.errorCategory)) << "\","
           << "\"error_code\":\"" << escapeJson(e.errorCode) << "\","
           << "\"error_message\":\"" << escapeJson(e.errorMessage) << "\""
           << "}\n";
}

}  // namespace rk_win

