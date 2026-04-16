#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

struct ArcFaceEmbedderConfig {
    enum class BackendType { OpenCvDnn = 1, Ncnn = 2, Qualcomm = 3 };

    BackendType backend = BackendType::OpenCvDnn;

    int inputW = 112;
    int inputH = 112;
    float scale = 1.0f / 127.5f;
    float meanB = 127.5f;
    float meanG = 127.5f;
    float meanR = 127.5f;
    bool swapRB = true;

    std::string opencvModel;
    std::string opencvConfig;
    std::string opencvFramework;
    std::string opencvOutput;
    std::string opencvInput;
    int opencvBackend = 0;
    int opencvTarget = 0;

    std::string ncnnParam;
    std::string ncnnBin;
    std::string ncnnInput = "data";
    std::string ncnnOutput = "output";
    int ncnnThreads = 1;
    bool ncnnLightmode = true;

    // [Qualcomm SDK Placeholder]
    std::string qualcommModel;
    std::string qualcommDelegate;

    std::uint32_t modelVersion = 1;
    std::uint32_t preprocessVersion = 1;
};

struct ArcFaceEmbedding {
    static constexpr int kDim = 512;

    std::uint32_t modelVersion = 1;
    std::uint32_t preprocessVersion = 1;
    std::vector<float> values;
};

class ArcFaceEmbedder {
public:
    static constexpr std::uint32_t kApiVersion = 1;

    bool initialize(const ArcFaceEmbedderConfig& cfg, std::string* err);
    bool isInitialized() const;

    std::optional<ArcFaceEmbedding> embedAlignedFaceBgr(const cv::Mat& alignedFaceBgr, std::string* err) const;

private:
    ArcFaceEmbedderConfig cfg_{};
    bool inited_ = false;

    struct OpenCvDnnState;
    std::shared_ptr<OpenCvDnnState> ocv_;

    struct NcnnState;
    std::shared_ptr<NcnnState> ncnn_;
};
