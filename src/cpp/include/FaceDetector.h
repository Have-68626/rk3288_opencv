#pragma once

#include "FaceDetections.h"

#include <opencv2/core.hpp>

#include <memory>
#include <string>
#include <vector>

/**
 * @brief Unified face detector interface for ModelRegistry.
 *
 * All detector implementations (YOLO Face, SCRFD, DNN SSD, Cascade LBP)
 * SHALL implement this interface so they can be registered and created
 * through the ModelRegistry singleton.
 */
class FaceDetector {
public:
    virtual ~FaceDetector() = default;

    /**
     * @brief Load model from the given path.
     * @param modelPath Path to the model file.
     * @param err Output error message on failure.
     * @return true if model loaded successfully.
     */
    virtual bool load(const std::string& modelPath, std::string& err) = 0;

    /**
     * @brief Detect faces in a BGR image.
     * @param bgr Input BGR image.
     * @param err Output error message on failure.
     * @return Detected faces.
     */
    virtual FaceDetections detect(const cv::Mat& bgr, std::string& err) = 0;

    /**
     * @brief Returns a human-readable name for this detector instance.
     */
    virtual const char* name() const = 0;
};
