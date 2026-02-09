/**
 * @file main.cpp
 * @brief Application entry point.
 * 
 * Initializes the AI Engine and starts the main processing loop.
 * Handles signal interruption for graceful shutdown.
 */
#include "Engine.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <string>

// Global pointer for signal handler
std::atomic<Engine*> g_engine(nullptr);

void signalHandler(int signum) {
    std::cout << "\nInterrupt signal (" << signum << ") received." << std::endl;
    Engine* engine = g_engine.load();
    if (engine) {
        engine->stop();
    }
}

int main(int argc, char** argv) {
    std::cout << "Starting RK3288 AI Engine (CLI Mode)..." << std::endl;

    // Parse command line arguments
    int cameraId = 0;
    if (argc > 1) {
        try {
            cameraId = std::stoi(argv[1]);
            std::cout << "Using Camera ID: " << cameraId << std::endl;
        } catch (...) {
            std::cerr << "Invalid camera ID provided. Using default (0)." << std::endl;
        }
    }

    // Register signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    Engine engine;
    g_engine.store(&engine);

    if (!engine.initialize(cameraId)) {
        std::cerr << "Engine initialization failed." << std::endl;
        return 1;
    }

    // Run the main loop (blocking)
    engine.run();

    std::cout << "Engine stopped. Exiting." << std::endl;
    return 0;
}
