/**
 * @file EventManager.cpp
 * @brief Implementation of EventManager class.
 */
#include "EventManager.h"
#include "Storage.h"
#include "Config.h"
#include <sstream>
#include <iomanip>
#include <ctime>
#include <random>

EventManager::EventManager() {}

void EventManager::logEvent(const std::string& type, const std::string& description, const std::string& snapshotPath) {
    AppEvent evt;
    evt.eventId = generateUniqueId();
    evt.type = type;
    evt.description = description;
    evt.timestamp = std::time(nullptr);
    evt.snapshotPath = snapshotPath;

    std::string jsonLog = formatEventJson(evt);
    
    // Save to daily log file
    std::string logFile = Config::CACHE_DIR + "events_" + std::to_string(evt.timestamp / 86400) + ".json";
    
    // Ensure cache dir exists
    Storage::ensureDirectory(Config::CACHE_DIR);
    
    Storage::appendLog(logFile, jsonLog);
}

std::string EventManager::formatEventJson(const AppEvent& event) {
    /*
     * [Performance Optimization - string formatting]
     * Why: Avoid high overhead of std::stringstream (virtual calls and locale overhead).
     * Impact: Significantly reduces CPU usage and memory fragmentation during high-frequency event logging.
     * Rollback: Revert to std::stringstream concatenation.
     */
    std::string s;
    s.reserve(64 + event.eventId.size() + event.type.size() + event.description.size() + 20 + event.snapshotPath.size());
    s += "{\"id\": \"";
    s += event.eventId;
    s += "\", \"type\": \"";
    s += event.type;
    s += "\", \"desc\": \"";
    s += event.description;
    s += "\", \"ts\": ";
    s += std::to_string(event.timestamp);
    s += ", \"img\": \"";
    s += event.snapshotPath;
    s += "\"}";
    return s;
}

std::string EventManager::generateUniqueId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static const char* digits = "0123456789abcdef";

    std::string uuid = "";
    for (int i = 0; i < 8; ++i) uuid += digits[dis(gen)];
    return uuid;
}
