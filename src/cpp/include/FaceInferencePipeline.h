#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FaceInferRequest {
    std::string imagePath;

    std::string yoloBackend = "opencv";
    std::string yoloModelPath;
    std::string yoloConfigPath;
    std::string yoloFramework;
    std::string yoloOutputName;
    int yoloInputW = 320;
    int yoloInputH = 320;
    float yoloScoreThreshold = 0.25f;
    float yoloNmsIouThreshold = 0.45f;
    bool yoloEnableKeypoints5 = true;
    bool yoloLetterbox = true;
    bool yoloSwapRB = true;
    float yoloScale = 1.0f / 255.0f;
    int yoloMeanB = 0;
    int yoloMeanG = 0;
    int yoloMeanR = 0;
    int yoloOpenCvBackend = 0;
    int yoloOpenCvTarget = 0;

    std::string arcBackend = "opencv";
    std::string arcModelPath;
    std::string arcConfigPath;
    std::string arcFramework;
    std::string arcOutputName;
    std::string arcInputName;
    int arcInputW = 112;
    int arcInputH = 112;
    std::uint32_t arcModelVersion = 1;
    std::uint32_t arcPreprocessVersion = 1;

    float acceptThreshold = 0.35f;
    std::string thresholdVersionId = "thr_v1";
    int consecutivePassesToTrigger = 1;

    std::string galleryDir;
    std::size_t topK = 5;

    std::string faceSelectPolicy = "score_area";

    bool fakeDetect = false;
    bool fakeEmbedding = false;
};

struct FaceInferOutcome {
    bool ok = false;
    int errorCode = 0;
    std::string stage;
    std::string message;

    std::string json;
    std::string auditDir;
    std::string auditFilename;
};

FaceInferOutcome runFaceInferOnce(const FaceInferRequest& req);
