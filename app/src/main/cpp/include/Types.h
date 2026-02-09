/**
 * @file Types.h
 * @brief Shared data structures and type definitions.
 * 
 * Decouples modules by providing common data types without circular dependencies.
 */
#pragma once

#include <string>
#include <vector>
#include <chrono>

/**
 * @brief Represents the identity of a recognized person.
 */
struct PersonIdentity {
    std::string id;         ///< Unique identifier
    std::string name;       ///< Display name
    float confidence;       ///< Recognition confidence (0.0 - 1.0)
    bool isAuthenticated;   ///< Whether the confidence met the threshold
};

/**
 * @brief Enumeration for video monitoring modes.
 */
enum class MonitoringMode {
    CONTINUOUS,     ///< Always process frames
    MOTION_TRIGGERED ///< Only process when motion is detected
};

/**
 * @brief Represents a system event for logging.
 */
struct AppEvent {
    std::string eventId;        ///< Unique event ID
    std::string type;           ///< Event type (e.g., "AUTH_SUCCESS", "MOTION_DETECTED")
    std::string description;    ///< Human-readable description
    long long timestamp;        ///< Unix timestamp in milliseconds
    std::string snapshotPath;   ///< Path to associated image file (optional)
};
