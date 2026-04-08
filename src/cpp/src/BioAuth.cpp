/**
 * @file BioAuth.cpp
 * @brief Implementation of BioAuth class.
 */
#include "BioAuth.h"
#include "Config.h"
#include <opencv2/imgproc.hpp>
#include <iostream>

BioAuth::BioAuth() : isModelLoaded(false) {
#ifdef HAS_OPENCV_FACE
    // Create LBPH Face Recognizer
    // Radius=1, Neighbors=8, GridX=8, GridY=8, Threshold=DBL_MAX
    faceRecognizer = cv::face::LBPHFaceRecognizer::create();
#endif
}

bool BioAuth::initialize(const std::string& cascadePath, const std::string& modelPath) {
    if (!faceCascade.load(cascadePath)) {
        std::cerr << "Error loading face cascade: " << cascadePath << std::endl;
        return false;
    }

#ifdef HAS_OPENCV_FACE
    if (!modelPath.empty()) {
        try {
            faceRecognizer->read(modelPath);
            isModelLoaded = true;
        } catch (const cv::Exception& e) {
            std::cerr << "Error loading face model: " << e.what() << std::endl;
            // Non-fatal if we intend to train later
        }
    }
#else
    if (!modelPath.empty()) {
        std::cerr << "Warning: Face recognition model path provided but OpenCV face module is not available." << std::endl;
    }
#endif
    return true;
}

void BioAuth::train(const std::vector<cv::Mat>& images, const std::vector<int>& labels) {
#ifdef HAS_OPENCV_FACE
    if (images.empty()) return;
    faceRecognizer->train(images, labels);
    isModelLoaded = true;
#else
    std::cerr << "Error: train() called but OpenCV face module is not available." << std::endl;
#endif
}

bool BioAuth::verify(const cv::Mat& frame, PersonIdentity& outIdentity) {
    std::vector<cv::Rect> faces;
    cv::Rect mainFace;
    return verify(frame, outIdentity, faces, mainFace);
}

bool BioAuth::verify(const cv::Mat& frame,
                     PersonIdentity& outIdentity,
                     std::vector<cv::Rect>& outFaces,
                     cv::Rect& outMainFace) {
    outFaces.clear();
    outMainFace = cv::Rect();

    std::vector<cv::Rect> faces;
    cv::Mat gray;

    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    cv::equalizeHist(gray, gray);

    faceCascade.detectMultiScale(gray, faces, 1.1, 3, 0, cv::Size(60, 60));

    if (faces.empty()) {
        return false;
    }

    cv::Rect largestFace = faces[0];
    for (const auto& face : faces) {
        if (face.area() > largestFace.area()) {
            largestFace = face;
        }
    }
    outFaces = faces;
    outMainFace = largestFace;

    if (isModelLoaded) {
#ifdef HAS_OPENCV_FACE
        int label = -1;
        double confidence = 0.0;
        
        // LBPH returns a "distance" metric, not probability. 
        // Lower distance = better match. 
        // 0 is perfect match. Usually < 50 is a good match.
        faceRecognizer->predict(gray(largestFace), label, confidence);

        outIdentity.id = std::to_string(label);
        
        // Normalize confidence roughly for the interface (0.0 - 1.0)
        // Assuming distance 100 is 0.0 confidence and 0 is 1.0
        float normConf = std::max(0.0f, static_cast<float>((100.0 - confidence) / 100.0));
        
        outIdentity.confidence = normConf;
        outIdentity.isAuthenticated = (normConf >= Config::BIO_AUTH_THRESHOLD);
#else
        outIdentity.id = "unknown_no_module";
        outIdentity.confidence = 0.0f;
        outIdentity.isAuthenticated = false;
#endif
    } else {
        // Mock behavior if no model loaded
        outIdentity.id = "unknown";
        outIdentity.confidence = 0.0f;
        outIdentity.isAuthenticated = false;
    }

    return true;
}
