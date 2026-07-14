/**
 * @file EventManager.cpp
 * @brief Implementation of EventManager class.
 */
#include "Storage.h"
#include "Config.h"
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <ctime>
#include <random>

#include "EventManager.h"
using namespace rk_core;

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
    std::string logFile = rk_core::config::CACHE_DIR + "events_" + std::to_string(evt.timestamp / 86400) + ".jsonl";
    
    // Ensure cache dir exists
    Storage::ensureDirectory(rk_core::config::CACHE_DIR);
    
    Storage::appendLog(logFile, jsonLog);
}

static std::string escapeJson(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (unsigned char c : raw) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string EventManager::formatEventJson(const AppEvent& event) {
    std::string s;
    s.reserve(64 + event.eventId.size() + event.type.size() + event.description.size() + 20 + event.snapshotPath.size());
    s += "{\"id\": \"";
    s += escapeJson(event.eventId);
    s += "\", \"type\": \"";
    s += escapeJson(event.type);
    s += "\", \"desc\": \"";
    s += escapeJson(event.description);
    s += "\", \"ts\": ";
    s += std::to_string(event.timestamp);
    s += ", \"img\": \"";
    s += escapeJson(event.snapshotPath);
    s += "\"}";
    return s;
}

std::string EventManager::generateUniqueId() {
    static thread_local std::mt19937 gen(std::random_device{}());
    static thread_local std::uniform_int_distribution<uint32_t> dis;
    static std::atomic<uint32_t> s_counter{0};
    static const char hex_chars[] = "0123456789abcdef";

    // Combine random 32-bit with atomic counter to reduce collision probability
    uint32_t v = dis(gen) ^ s_counter.fetch_add(1, std::memory_order_relaxed);
    std::string uuid(8, '0');
    for (int i = 0; i < 8; ++i) {
        uuid[i] = hex_chars[v & 0x0f];
        v >>= 4;
    }
    return uuid;
}
