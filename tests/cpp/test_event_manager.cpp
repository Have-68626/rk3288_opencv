#include "EventManager.h"
#include <gtest/gtest.h>
#include <set>
#include <string>

using namespace rk_core;
namespace rk_core {

// Stubs for Storage methods used by EventManager to avoid linking full Storage class
// which requires opencv
#include "Storage.h"
bool Storage::ensureDirectory(const std::string& path) { return true; }
bool Storage::appendLog(const std::string& filename, const std::string& content) { return true; }
void Storage::cleanupOldData(const std::string& directoryPath, int daysToKeep) {}
bool Storage::saveImage(const std::string& filename, const cv::Mat& frame) { return true; }
bool Storage::hasEnoughSpace(const std::string& path, size_t minBytes) { return true; }

TEST(EventManager, FormatJson) {
    EventManager manager;
    AppEvent evt;
    evt.eventId = "abc12345";
    evt.type = "AUTH_FAIL";
    evt.description = "Failed login attempt";
    evt.timestamp = 1672531200;
    evt.snapshotPath = "/tmp/snap.jpg";

    std::string json = manager.formatEventJson(evt);

    EXPECT_NE(json.find("\"id\": \"abc12345\""), std::string::npos);
    EXPECT_NE(json.find("\"type\": \"AUTH_FAIL\""), std::string::npos);
    EXPECT_NE(json.find("\"desc\": \"Failed login attempt\""), std::string::npos);
    EXPECT_NE(json.find("\"ts\": 1672531200"), std::string::npos);
    EXPECT_NE(json.find("\"img\": \"/tmp/snap.jpg\""), std::string::npos);
}

TEST(EventManager, UniqueId) {
    EventManager manager;
    std::set<std::string> ids;

    for (int i = 0; i < 100; ++i) {
        std::string id = manager.generateUniqueId();

        EXPECT_EQ(id.length(), 8);
        if (id.length() != 8) {
            // Early continue to avoid out-of-bounds checks on short strings
            continue;
        }

        // Check charset (hexadecimal)
        for (char c : id) {
            EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }

        // Check uniqueness
        EXPECT_EQ(ids.find(id), ids.end());
        ids.insert(id);
    }
}
} // namespace rk_core
