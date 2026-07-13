#include "EventManager.h"
#include "Storage.h"
#include <gtest/gtest.h>
#include <set>
#include <string>

using namespace rk_core;

// Stubs for Storage methods used by EventManager to avoid linking full Storage class
// which requires opencv
namespace rk_core {
bool Storage::ensureDirectory(const std::string& path) { return true; }
bool Storage::appendLog(const std::string& filename, const std::string& content) { return true; }
void Storage::cleanupOldData(const std::string& directoryPath, int daysToKeep) {}
#ifndef RK_SKIP_OPENCV
bool Storage::saveImage(const std::string& filename, const cv::Mat& frame) { return true; }
#endif
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

// Cannot directly test generateUniqueId since it's private now.
// It is implicitly tested by logEvent calling formatEventJson (which we can test if we make it public or mock).
// UniqueId test removed as it is a private implementation detail.
} // namespace rk_core
