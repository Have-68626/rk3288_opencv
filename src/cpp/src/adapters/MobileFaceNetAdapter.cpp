#include "adapters/MobileFaceNetAdapter.h"

#include "NativeLog.h"

#include <filesystem>

#include <cstring>

#include <opencv2/imgproc.hpp>

MobileFaceNetAdapter::MobileFaceNetAdapter() = default;

MobileFaceNetAdapter::~MobileFaceNetAdapter() = default;

bool MobileFaceNetAdapter::load(const std::string& modelPath, std::string& err) {
    loaded_ = false;

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    std::string paramPath;
    std::string binPath;
    std::string ext = std::filesystem::path(modelPath).extension().string();

    if (ext == ".param") {
        paramPath = modelPath;
        binPath = (std::filesystem::path(modelPath).parent_path() / std::filesystem::path(modelPath).stem().replace_extension(".bin")).string();
    } else if (ext == ".bin") {
        binPath = modelPath;
        paramPath = (std::filesystem::path(modelPath).parent_path() / std::filesystem::path(modelPath).stem().replace_extension(".param")).string();
    } else {
        // Treat bare path as base name, append .param and .bin
        paramPath = modelPath + ".param";
        binPath = modelPath + ".bin";
    }

    if (net_.load_param(paramPath.c_str()) != 0) {
        err = "mobilefacenet: load_param failed: " + paramPath;
        return false;
    }
    if (net_.load_model(binPath.c_str()) != 0) {
        err = "mobilefacenet: load_model failed: " + binPath;
        return false;
    }

    net_.opt.lightmode = true;
    net_.opt.num_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (net_.opt.num_threads < 1) net_.opt.num_threads = 1;

    currentName_ = "mobilefacenet_ncnn";
    loaded_ = true;
    rklog::logInfo("MobileFaceNetAdapter", "load", "Loaded ncnn model: " + paramPath);
    return true;
#else
    (void)modelPath;
    err = "mobilefacenet: ncnn not available (RK_HAVE_NCNN=0)";
    return false;
#endif
}

std::optional<std::vector<float>> MobileFaceNetAdapter::embed(const cv::Mat& alignedFaceBgr, std::string& err) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!loaded_) {
            err = "mobilefacenet: not loaded";
            return std::nullopt;
        }
    }

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    cv::Mat resized;
    cv::resize(alignedFaceBgr, resized, cv::Size(inputW_, inputH_), 0, 0, cv::INTER_LINEAR);

    cv::Mat src = resized;
    if (!src.isContinuous()) {
        src = src.clone();
    }

    ncnn::Mat in = ncnn::Mat::from_pixels(src.data, ncnn::Mat::PIXEL_BGR2RGB, inputW_, inputH_);

    float meanVals[3] = {0.0f, 0.0f, 0.0f};
    float normVals[3] = {1.0f / 127.5f, 1.0f / 127.5f, 1.0f / 127.5f};
    in.substract_mean_normalize(meanVals, normVals);

    ncnn::Extractor ex = net_.create_extractor();

    if (ex.input("data", in) != 0) {
        err = "mobilefacenet: input failed";
        return std::nullopt;
    }

    ncnn::Mat out;
    if (ex.extract("output", out) != 0) {
        err = "mobilefacenet: extract failed";
        return std::nullopt;
    }

    const std::size_t total = static_cast<std::size_t>(out.total());
    if (total != 128) {
        err = "mobilefacenet: expected output dim 128, got " + std::to_string(total);
        return std::nullopt;
    }

    std::vector<float> embedding(128);
    std::memcpy(embedding.data(), (const float*)out.data, 128 * sizeof(float));

    // L2 normalize for correct cosine similarity comparison
    float sqSum = 0.0f;
    for (float v : embedding) sqSum += v * v;
    if (sqSum > 0.0f) {
        float inv = 1.0f / std::sqrt(sqSum);
        for (float& v : embedding) v *= inv;
    }

    return embedding;
#else
    (void)err;
    return std::nullopt;
#endif
}

const char* MobileFaceNetAdapter::name() const {
    std::lock_guard<std::mutex> lock(mu_);
    return currentName_.c_str();
}
