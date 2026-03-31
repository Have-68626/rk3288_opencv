#pragma once

#include <cstdint>
#include "Compat.h"
#include <fstream>
#include <mutex>
#if __has_include(<optional>)
#include <optional>
#endif
#if !defined(__cpp_lib_optional)
namespace std {
template <class T>
class optional;
}
#endif
#include <string>
#include <vector>

namespace rk_win {

enum class ErrorCategory {
    None = 0,
    PrivacyDenied,
    DeviceNotFound,
    DeviceBusy,
    FormatNotSupported,
    BackendFailure,
    Unknown,
};

struct FaceLogEntry {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::string personId;
    double distance = 0.0;
    double confidence = 0.0;
};

struct FrameLogEntry {
    std::string tsIso8601;
    std::string cameraName;
    std::string cameraId;
    std::uint64_t frameIndex = 0;
    int frameWidth = 0;
    int frameHeight = 0;
    double fps = 0.0;
    std::vector<FaceLogEntry> faces;
    ErrorCategory errorCategory = ErrorCategory::None;
    std::string errorCode;
    std::string errorMessage;
};

class StructuredLogger {
public:
    StructuredLogger() = default;

    bool open(const std::filesystem::path& dir, std::uint64_t maxBytes, int maxRollFiles);
    void close();

    void append(const FrameLogEntry& e);

private:
    void rollIfNeededLocked(const std::filesystem::path& path);
    static std::string escapeCsv(const std::string& s);
    static std::string escapeJson(const std::string& s);

    std::mutex mu_;
    std::filesystem::path dir_;
    std::filesystem::path csvPath_;
    std::filesystem::path jsonlPath_;
    std::uint64_t maxBytes_ = 0;
    int maxRollFiles_ = 0;
    std::ofstream csv_;
    std::ofstream jsonl_;
    bool headerWritten_ = false;
};

std::string nowIso8601Local();
std::string toString(ErrorCategory c);

}  // namespace rk_win

