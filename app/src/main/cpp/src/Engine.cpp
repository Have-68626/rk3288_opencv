/**
 * @file Engine.cpp
 * @brief Implementation of Engine class.
 */
#include "Engine.h"
#include "Config.h"
#include "Storage.h"
#include "NativeLog.h"
#include <iostream>
#include <thread>

Engine::Engine() 
    : isRunning(false), currentMode(MonitoringMode::CONTINUOUS), frameCount(0), lastStatTime(0) {
    videoManager = std::make_unique<VideoManager>();
    motionDetector = std::make_unique<MotionDetector>();
    bioAuth = std::make_unique<BioAuth>();
    eventManager = std::make_unique<EventManager>();
}

Engine::~Engine() {
    stop();
}

bool Engine::initialize(int cameraId) {
    RKLOG_ENTER("Engine");
    // 1. Ensure storage
    if (!Storage::ensureDirectory(Config::CACHE_DIR)) {
        std::cerr << "Failed to init storage." << std::endl;
        rklog::logError("Engine", __func__, "Failed to init storage");
        return false;
    }
    
    // 2. Cleanup old data
    Storage::cleanupOldData(Config::CACHE_DIR, Config::OFFLINE_CACHE_DAYS);

    // 3. Init Camera
    // Note: On Android with JNI, camera ID 0 is usually back camera.
    // If opening fails, it might be due to permissions or index.
    if (!videoManager->open(cameraId)) {
        std::cerr << "Failed to open camera " << cameraId << "." << std::endl;
        rklog::logError("Engine", __func__, "Failed to open camera");
        // Don't return false yet, retry or allow running without camera for UI testing
        // return false; 
    }

    return true;
}

bool Engine::initialize(const std::string& filePath) {
    RKLOG_ENTER("Engine");
    // 1. Ensure storage
    if (!Storage::ensureDirectory(Config::CACHE_DIR)) {
        std::cerr << "Failed to init storage." << std::endl;
        rklog::logError("Engine", __func__, "Failed to init storage");
        return false;
    }
    
    // 2. Cleanup old data
    Storage::cleanupOldData(Config::CACHE_DIR, Config::OFFLINE_CACHE_DAYS);

    // 3. Init Mock Source
    if (!videoManager->open(filePath)) {
        std::cerr << "Failed to open mock file: " << filePath << std::endl;
        rklog::logError("Engine", __func__, "Failed to open mock file");
        return false;
    }

    return true;
}

void Engine::run() {
    RKLOG_ENTER("Engine");
    isRunning = true;
    cv::Mat frame;

    std::cout << "Engine started." << std::endl;

    while (isRunning) {
        // Enforce loop timing if needed, but VideoManager controls capture rate.
        
        if (videoManager->getLatestFrame(frame)) {
            processFrame(frame);
            
            // Stats
            frameCount++;
            long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            if (now - lastStatTime > 1000) {
                // std::cout << "FPS: " << frameCount << std::endl;
                frameCount = 0;
                lastStatTime = now;
            }
        } else {
            // No new frame, sleep briefly to avoid busy wait
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void Engine::stop() {
    RKLOG_ENTER("Engine");
    isRunning = false;
    videoManager->close();
}

void Engine::setMode(MonitoringMode mode) {
    RKLOG_ENTER("Engine");
    currentMode = mode;
    std::cout << "Switched to mode: " << (mode == MonitoringMode::CONTINUOUS ? "Continuous" : "Motion") << std::endl;
}

void Engine::processFrame(const cv::Mat& inputFrame) {
    // Clone frame for drawing
    cv::Mat debugFrame = inputFrame.clone();

    // 1. Motion Detection check for Non-Continuous mode
    if (currentMode == MonitoringMode::MOTION_TRIGGERED) {
        if (!motionDetector->detect(inputFrame)) {
            // Even if no motion, we update render frame
            std::lock_guard<std::mutex> lock(renderMutex);
            renderFrame = debugFrame;
            return; 
        }
        // Visualize motion (optional: draw contours)
        cv::putText(debugFrame, "MOTION DETECTED", cv::Point(20, 40), 
            cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);
    }

    // 2. Biometric Authentication
    PersonIdentity identity;
    bool faceDetected = bioAuth->verify(inputFrame, identity);

    if (faceDetected) {
        cv::Scalar color = identity.isAuthenticated ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
        // We assume verify() doesn't give us rects back directly in this API, 
        // but for now we'll just print status text. 
        // Improvement: Update BioAuth to return Face Rects.
        
        std::string statusText = identity.isAuthenticated ? 
            "Verified: " + identity.id : "Unknown";
        
        cv::putText(debugFrame, statusText, cv::Point(20, 80), 
            cv::FONT_HERSHEY_SIMPLEX, 0.8, color, 2);

        if (identity.isAuthenticated) {
            std::cout << "User Verified: " << identity.id << " (" << identity.confidence << ")" << std::endl;
        } else {
            // 3. Abnormal Event: Unknown person
            handleAbnormalEvent("AUTH_FAIL", "Unknown person detected", inputFrame);
        }
    }

    // Update the render frame safely
    {
        std::lock_guard<std::mutex> lock(renderMutex);
        renderFrame = debugFrame;
    }
}

bool Engine::getRenderFrame(cv::Mat& outFrame) {
    std::lock_guard<std::mutex> lock(renderMutex);
    if (renderFrame.empty()) return false;
    renderFrame.copyTo(outFrame);
    return true;
}

void Engine::handleAbnormalEvent(const std::string& type, const std::string& desc, const cv::Mat& evidence) {
    RKLOG_ENTER("Engine");
    std::string timestamp = std::to_string(std::time(nullptr));
    std::string imgPath = Config::CACHE_DIR + type + "_" + timestamp + ".jpg";
    
    // Save evidence asynchronously or quickly
    if (Storage::saveImage(imgPath, evidence)) {
        eventManager->logEvent(type, desc, imgPath);
        std::cout << "Event Logged: " << desc << std::endl;
    }
}
