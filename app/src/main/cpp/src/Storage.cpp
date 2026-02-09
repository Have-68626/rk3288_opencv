/**
 * @file Storage.cpp
 * @brief Implementation of Storage class.
 */
#include "Storage.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <ctime>
#include <cerrno>
#include <opencv2/imgcodecs.hpp>

// Helper to get file age in days
static double getFileAgeDays(time_t lastModified) {
    time_t now = time(nullptr);
    double seconds = difftime(now, lastModified);
    return seconds / (60.0 * 60.0 * 24.0);
}

bool Storage::ensureDirectory(const std::string& path) {
    if (path.empty()) return false;

    std::string tempPath = path;
    // Remove trailing slash if present to simplify logic
    if (tempPath.back() == '/') {
        tempPath.pop_back();
    }

    std::string currentPath;
    for (size_t i = 0; i < tempPath.length(); ++i) {
        currentPath += tempPath[i];
        if (tempPath[i] == '/' && currentPath.length() > 1) {
            // Check/Create intermediate directory
            struct stat info;
            if (stat(currentPath.c_str(), &info) != 0) {
                if (mkdir(currentPath.c_str(), 0777) != 0 && errno != EEXIST) {
                    std::cerr << "Error creating directory: " << currentPath << " (errno: " << errno << ")" << std::endl;
                    return false;
                }
            } else if (!(info.st_mode & S_IFDIR)) {
                // If it exists but is not a directory, that's a problem
                // But for intermediate paths, we might want to fail or just continue hoping it's a symlink?
                // Standard strict check:
                std::cerr << "Path exists but is not a directory: " << currentPath << std::endl;
                return false;
            }
        }
    }

    // Create the final directory
    struct stat info;
    if (stat(tempPath.c_str(), &info) != 0) {
        if (mkdir(tempPath.c_str(), 0777) != 0 && errno != EEXIST) {
            std::cerr << "Error creating directory: " << tempPath << " (errno: " << errno << ")" << std::endl;
            return false;
        }
    } else if (!(info.st_mode & S_IFDIR)) {
        std::cerr << "Path exists but is not a directory: " << tempPath << std::endl;
        return false;
    }
    return true;
}

void Storage::cleanupOldData(const std::string& directoryPath, int daysToKeep) {
    DIR* dir = opendir(directoryPath.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_REG) { // Regular file
            std::string fullPath = directoryPath + "/" + entry->d_name;
            struct stat result;
            if (stat(fullPath.c_str(), &result) == 0) {
                if (getFileAgeDays(result.st_mtime) > daysToKeep) {
                    remove(fullPath.c_str());
                    std::cout << "Removed old file: " << fullPath << std::endl;
                }
            }
        }
    }
    closedir(dir);
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
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) != 0) {
        return false;
    }
    // Available blocks * block size
    size_t available = stat.f_bavail * stat.f_frsize;
    return available >= minBytes;
}
