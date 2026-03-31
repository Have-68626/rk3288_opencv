/**
 * @file Config.h
 * @brief Global configuration constants for the RK3288 AI Engine.
 * 
 * Defines resource limits, performance thresholds, and application settings.
 */
#pragma once

#include <string>

namespace Config {

    // --- Hardware Constraints ---
    /** Maximum memory usage allowed (512MB). */
    const size_t MAX_MEMORY_USAGE_BYTES = 512 * 1024 * 1024;
    
    /** Peak CPU usage target percentage. */
    const int MAX_CPU_USAGE_PERCENT = 60;
    
    /** Video latency threshold in milliseconds. */
    const int VIDEO_LATENCY_THRESHOLD_MS = 300;

    // --- Application Settings ---
    /** Number of days to keep offline cache. */
    const int OFFLINE_CACHE_DAYS = 7;
    
    /** Minimum confidence score for biometric authentication (0.0 - 1.0). */
    const double BIO_AUTH_THRESHOLD = 0.92;

    /** Directory for application cache and logs. */
    const std::string CACHE_DIR = "cache/";

    // --- Video Settings ---
    /** Capture width (VGA optimized for performance). */
    const int FRAME_WIDTH = 640;
    
    /** Capture height. */
    const int FRAME_HEIGHT = 480;
    
    /** Target frames per second. */
    const int TARGET_FPS = 30;

    /** Motion detection threshold (pixel difference). */
    const int MOTION_THRESHOLD = 25;

    /** Minimum area for motion to be considered valid. */
    const int MIN_MOTION_AREA = 500;
}
