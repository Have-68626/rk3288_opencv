/**
 * @file Storage.h
 * @brief Handles file I/O and cache management.
 * 
 * Provides static utility methods for file operations, including 
 * directory creation, file saving, and automatic cleanup of old data.
 */
#pragma once

#include <string>
#include <vector>

#ifndef RK_SKIP_OPENCV
#include <opencv2/core.hpp>
#else
namespace cv { class Mat; }
#endif

class Storage {
public:
    /**
     * @brief Ensures that a directory exists.
     * @param path The absolute path to the directory.
     * @return true if directory exists or was created, false on failure.
     */
    static bool ensureDirectory(const std::string& path);

    /**
     * @brief Removes files older than the specified number of days in the given directory.
     * @param directoryPath Path to the cache directory.
     * @param daysToKeep Max age of files in days.
     */
    static void cleanupOldData(const std::string& directoryPath, int daysToKeep);

    /**
     * @brief Saves an image frame to disk.
     * @param filename Full path including filename.
     * @param frame The OpenCV matrix to save.
     * @return true if successful.
     */
    static bool saveImage(const std::string& filename, const cv::Mat& frame);

    /**
     * @brief Appends text to a log file.
     * @param filename Full path to the log file.
     * @param content String content to append.
     * @return true if successful.
     */
    static bool appendLog(const std::string& filename, const std::string& content);

    /**
     * @brief Checks if there is sufficient disk space.
     * @param path Directory path to check.
     * @param minBytes Minimum required bytes.
     * @return true if space is available.
     */
    static bool hasEnoughSpace(const std::string& path, size_t minBytes = 1024 * 1024 * 50);
};
