/**
 * @file BioAuth.h
 * @brief Biometric authentication module.
 * 
 * Wraps OpenCV face detection and recognition algorithms.
 * Uses LBP Cascades for fast detection on ARM architecture.
 */
#pragma once

#include "Types.h"
#include <opencv2/objdetect.hpp>

// Check if face module is available (it's in opencv_contrib)
#if __has_include(<opencv2/face.hpp>)
#include <opencv2/face.hpp>
#define HAS_OPENCV_FACE
#endif

#include <memory>

class BioAuth {
public:
    BioAuth();
    
    enum class FaceSelectMode {
        MAIN_FACE = 0,
        MULTI_FACES = 1
    };

    /**
     * @brief Initializes the authentication models.
     * @param cascadePath Path to the XML cascade file for detection.
     * @param modelPath Path to the trained face recognition model.
     * @return true if initialization successful.
     */
    bool initialize(const std::string& cascadePath, const std::string& modelPath = "");

    /**
     * @brief Trains the recognizer with a set of images and labels.
     */
    void train(const std::vector<cv::Mat>& images, const std::vector<int>& labels);

    void setFaceSelectMode(FaceSelectMode mode) { faceMode = mode; }

    /**
     * @brief Verifies the identity of persons in the frame.
     * @param frame Input video frame.
     * @param outIdentity Output structure with result.
     * @return true if a valid face was detected (authentication status is in outIdentity).
     */
    bool verify(const cv::Mat& frame, PersonIdentity& outIdentity);

    bool verify(const cv::Mat& frame,
                PersonIdentity& outIdentity,
                std::vector<cv::Rect>& outFaces,
                cv::Rect& outMainFace);

private:
    cv::CascadeClassifier faceCascade;
#ifdef HAS_OPENCV_FACE
    cv::Ptr<cv::face::FaceRecognizer> faceRecognizer;
#endif
    bool isModelLoaded;
    FaceSelectMode faceMode = FaceSelectMode::MAIN_FACE;
};
