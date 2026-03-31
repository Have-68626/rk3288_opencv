/**
 * @file Storage.cpp
 * @brief Implementation of Storage class.
 */
#include "Storage.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <opencv2/imgcodecs.hpp>

bool Storage::ensureDirectory(const std::string& path) {
    if (path.empty()) return false;
#ifdef _WIN32
    try {
        std::filesystem::path p(path);
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        if (ec) return false;
        return std::filesystem::exists(p) && std::filesystem::is_directory(p);
    } catch (...) {
        return false;
    }
#else
    return std::filesystem::create_directories(path) || std::filesystem::exists(path);
#endif
}

void Storage::cleanupOldData(const std::string& directoryPath, int daysToKeep) {
    if (directoryPath.empty() || daysToKeep <= 0) return;
    std::error_code ec;
    if (!std::filesystem::exists(directoryPath, ec)) return;
    if (!std::filesystem::is_directory(directoryPath, ec)) return;

    auto now = std::chrono::system_clock::now();
    auto keep = std::chrono::hours(24LL * static_cast<long long>(daysToKeep));

    for (const auto& entry : std::filesystem::directory_iterator(directoryPath, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        auto ftime = entry.last_write_time(ec);
        if (ec) continue;

        auto sysTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());

        if (now - sysTime > keep) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

bool Storage::saveImage(const std::string& filename, const cv::Mat& frame) {
    if (frame.empty()) return false;
    try {
        return cv::imwrite(filename, frame);
    } catch (...) {
        return false;
    }
}

bool Storage::appendLog(const std::string& filename, const std::string& content) {
    std::ofstream outfile;
    outfile.open(filename, std::ios_base::app); // Append mode
    if (!outfile.is_open()) return false;
    outfile << content << std::endl;
    return true;
}

bool Storage::hasEnoughSpace(const std::string& path, size_t minBytes) {
    std::error_code ec;
    auto info = std::filesystem::space(path, ec);
    if (ec) return false;
    return static_cast<size_t>(info.available) >= minBytes;
}
