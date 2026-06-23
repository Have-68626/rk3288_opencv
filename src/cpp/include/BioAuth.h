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
#include <mutex>
#include <vector>

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
    bool initialize(const std::string& cascadePath, const std::string& modelPath = "",
                    double scaleFactor = 1.1, int minNeighbors = 3, int minFaceSize = 60);

    /**
     * @brief Trains the recognizer with a set of images and labels.
     */
    void train(const std::vector<cv::Mat>& images, const std::vector<int>& labels);

    void setFaceSelectMode(FaceSelectMode mode);

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

    struct FaceAuthResult {
        cv::Rect face;
        PersonIdentity identity;
        bool isMain = false;
    };

    bool verifyMulti(const cv::Mat& frame, std::vector<FaceAuthResult>& outResults, int maxFaces = 3, bool enableRecognition = true);

private:
    static float normalizeLbphConfidence(double distance);

    mutable std::mutex mu_;
    double cascadeScaleFactor_ = 1.1;
    int cascadeMinNeighbors_ = 3;
    int cascadeMinFaceSize_ = 60;
    cv::CascadeClassifier faceCascade;
#ifdef HAS_OPENCV_FACE
    cv::Ptr<cv::face::FaceRecognizer> faceRecognizer;
#endif
    bool isModelLoaded = false;
    FaceSelectMode faceMode = FaceSelectMode::MAIN_FACE;
};
