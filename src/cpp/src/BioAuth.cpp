#include "BioAuth.h"
#include "Config.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <iostream>

namespace {
constexpr int kCascadeFlags = 0;

cv::Rect clipToImage(const cv::Rect& r, int cols, int rows) {
    return r & cv::Rect(0, 0, cols, rows);
}
}

BioAuth::BioAuth() {
#ifdef HAS_OPENCV_FACE
    faceRecognizer = cv::face::LBPHFaceRecognizer::create();
#endif
}

bool BioAuth::initialize(const std::string& cascadePath, const std::string& modelPath,
                         double scaleFactor, int minNeighbors, int minFaceSize) {
    std::lock_guard<std::mutex> lock(mu_);
    cascadeScaleFactor_ = scaleFactor;
    cascadeMinNeighbors_ = minNeighbors;
    cascadeMinFaceSize_ = minFaceSize;
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
    std::lock_guard<std::mutex> lock(mu_);
    faceRecognizer->train(images, labels);
    isModelLoaded = true;
#else
    std::cerr << "Error: train() called but OpenCV face module is not available." << std::endl;
#endif
}

void BioAuth::setFaceSelectMode(FaceSelectMode mode) {
    std::lock_guard<std::mutex> lock(mu_);
    faceMode = mode;
}

float BioAuth::normalizeLbphConfidence(double distance) {
    // LBPH distance is unbounded: 0 = perfect match, no upper limit.
    // Use 1/(1+d) to map [0, +∞) → (0.0, 1.0].
    return static_cast<float>(1.0 / (1.0 + distance));
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

    {
        std::lock_guard<std::mutex> lock(mu_);
        faceCascade.detectMultiScale(gray, faces, cascadeScaleFactor_, cascadeMinNeighbors_, kCascadeFlags, cv::Size(cascadeMinFaceSize_, cascadeMinFaceSize_));
    }

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

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (isModelLoaded) {
#ifdef HAS_OPENCV_FACE
            int label = -1;
            double confidence = 0.0;

            faceRecognizer->predict(gray(clipToImage(largestFace, gray.cols, gray.rows)), label, confidence);

            outIdentity.id = std::to_string(label);
            outIdentity.confidence = normalizeLbphConfidence(confidence);
            outIdentity.isAuthenticated = (outIdentity.confidence >= Config::BIO_AUTH_THRESHOLD);
#else
            outIdentity.id = "unknown_no_module";
            outIdentity.confidence = 0.0f;
            outIdentity.isAuthenticated = false;
#endif
        } else {
            outIdentity.id = "unknown";
            outIdentity.confidence = 0.0f;
            outIdentity.isAuthenticated = false;
        }
    }

    return true;
}

bool BioAuth::verifyMulti(const cv::Mat& frame, std::vector<FaceAuthResult>& outResults, int maxFaces, bool enableRecognition) {
    outResults.clear();

    std::vector<cv::Rect> faces;
    cv::Mat gray;

    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    cv::equalizeHist(gray, gray);

    {
        std::lock_guard<std::mutex> lock(mu_);
        faceCascade.detectMultiScale(gray, faces, cascadeScaleFactor_, cascadeMinNeighbors_, kCascadeFlags, cv::Size(cascadeMinFaceSize_, cascadeMinFaceSize_));
    }

    if (faces.empty()) {
        return false;
    }

    cv::Rect largestFace = faces[0];
    for (const auto& face : faces) {
        if (face.area() > largestFace.area()) {
            largestFace = face;
        }
    }

    int clampedMaxFaces = std::max(1, maxFaces);
    struct IndexedRect {
        cv::Rect rect;
        int index;
    };
    std::vector<IndexedRect> indexedFaces;
    indexedFaces.reserve(faces.size());
    for (size_t i = 0; i < faces.size(); ++i) {
        indexedFaces.push_back({faces[i], static_cast<int>(i)});
    }

    auto comp = [](const IndexedRect& a, const IndexedRect& b) {
        int areaA = a.rect.area();
        int areaB = b.rect.area();
        if (areaA != areaB) return areaA > areaB;
        return a.index < b.index;
    };

    if (static_cast<int>(indexedFaces.size()) > clampedMaxFaces) {
        std::partial_sort(indexedFaces.begin(), indexedFaces.begin() + clampedMaxFaces, indexedFaces.end(), comp);
        indexedFaces.resize(clampedMaxFaces);
    } else {
        std::sort(indexedFaces.begin(), indexedFaces.end(), comp);
    }

    faces.clear();
    faces.reserve(indexedFaces.size());
    for (const auto& ir : indexedFaces) {
        faces.push_back(ir.rect);
    }

    outResults.reserve(faces.size());
    for (const auto& face : faces) {
        FaceAuthResult r;
        r.face = face;
        r.isMain = (face == largestFace);

        if (enableRecognition) {
            std::lock_guard<std::mutex> lock(mu_);
            if (isModelLoaded) {
#ifdef HAS_OPENCV_FACE
                int label = -1;
                double distance = 0.0;
                faceRecognizer->predict(gray(clipToImage(face, gray.cols, gray.rows)), label, distance);

                r.identity.id = (label >= 0) ? std::to_string(label) : "unknown";
                r.identity.confidence = normalizeLbphConfidence(distance);
                r.identity.isAuthenticated = (r.identity.confidence >= Config::BIO_AUTH_THRESHOLD);
#else
                r.identity.id = "unknown_no_module";
                r.identity.confidence = 0.0f;
                r.identity.isAuthenticated = false;
#endif
            } else {
                r.identity.id = "unknown";
                r.identity.confidence = 0.0f;
                r.identity.isAuthenticated = false;
            }
        } else {
            r.identity.id = "unknown";
            r.identity.confidence = 0.0f;
            r.identity.isAuthenticated = false;
        }

        outResults.push_back(std::move(r));
    }

    return true;
}
