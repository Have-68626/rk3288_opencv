/**
 * @file EventManager.h
 * @brief Manages system events and reporting.
 * 
 * Handles the creation of structured reports for abnormal events
 * and interfaces with the Storage module for persistence.
 */
#pragma once

#include "Types.h"
#include <string>

class EventManager {
public:
    EventManager();
    
    /**
     * @brief Logs an event to the system.
     * @param type Event category (e.g. "AUTH_FAIL").
     * @param description Detail message.
     * @param snapshotPath Optional path to image evidence.
     */
    void logEvent(const std::string& type, const std::string& description, const std::string& snapshotPath = "");

    /**
     * @brief Generates a JSON formatted report of the event.
     * @param event The event object.
     * @return JSON string.
     */
    std::string formatEventJson(const AppEvent& event);

private:
    std::string generateUniqueId();
};
