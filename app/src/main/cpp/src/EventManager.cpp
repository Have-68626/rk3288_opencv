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
    std::stringstream ss;
    ss << "{";
    ss << "\"id\": \"" << event.eventId << "\", ";
    ss << "\"type\": \"" << event.type << "\", ";
    ss << "\"desc\": \"" << event.description << "\", ";
    ss << "\"ts\": " << event.timestamp << ", ";
    ss << "\"img\": \"" << event.snapshotPath << "\"";
    ss << "}";
    return ss.str();
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
