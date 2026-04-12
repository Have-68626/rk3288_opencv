#include "EventManager.h"
#include <iostream>
#include <set>
#include <string>

// Stubs for Storage methods used by EventManager to avoid linking full Storage class
// which requires opencv
#include "Storage.h"
bool Storage::ensureDirectory(const std::string& path) { return true; }
bool Storage::appendLog(const std::string& filename, const std::string& content) { return true; }
void Storage::cleanupOldData(const std::string& directoryPath, int daysToKeep) {}
bool Storage::saveImage(const std::string& filename, const cv::Mat& frame) { return true; }
bool Storage::hasEnoughSpace(const std::string& path, size_t minBytes) { return true; }

bool test_event_manager_format_json() {
    EventManager manager;
    AppEvent evt;
    evt.eventId = "abc12345";
    evt.type = "AUTH_FAIL";
    evt.description = "Failed login attempt";
    evt.timestamp = 1672531200;
    evt.snapshotPath = "/tmp/snap.jpg";

    std::string json = manager.formatEventJson(evt);

    // Check for substrings instead of exact match
    if (json.find("\"id\": \"abc12345\"") == std::string::npos) {
        std::cerr << "test_event_manager_format_json failed: missing id" << std::endl;
        return false;
    }
    if (json.find("\"type\": \"AUTH_FAIL\"") == std::string::npos) {
        std::cerr << "test_event_manager_format_json failed: missing type" << std::endl;
        return false;
    }
    if (json.find("\"desc\": \"Failed login attempt\"") == std::string::npos) {
        std::cerr << "test_event_manager_format_json failed: missing desc" << std::endl;
        return false;
    }
    if (json.find("\"ts\": 1672531200") == std::string::npos) {
        std::cerr << "test_event_manager_format_json failed: missing timestamp" << std::endl;
        return false;
    }
    if (json.find("\"img\": \"/tmp/snap.jpg\"") == std::string::npos) {
        std::cerr << "test_event_manager_format_json failed: missing snapshotPath" << std::endl;
        return false;
    }

    return true;
}

bool test_event_manager_unique_id() {
    EventManager manager;
    std::set<std::string> ids;

    for (int i = 0; i < 100; ++i) {
        std::string id = manager.generateUniqueId(); // Requires friend access

        // Check length
        if (id.length() != 8) {
            std::cerr << "test_event_manager_unique_id failed: incorrect length" << std::endl;
            return false;
        }

        // Check charset (hexadecimal)
        for (char c : id) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                std::cerr << "test_event_manager_unique_id failed: invalid charset" << std::endl;
                return false;
            }
        }

        // Check uniqueness
        if (ids.find(id) != ids.end()) {
            std::cerr << "test_event_manager_unique_id failed: collision detected" << std::endl;
            return false;
        }
        ids.insert(id);
    }

    return true;
}
