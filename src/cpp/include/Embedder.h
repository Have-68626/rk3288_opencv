#pragma once

#if __has_include(<opencv2/core.hpp>) && !defined(RK_SKIP_OPENCV)
#include <opencv2/core.hpp>
#else
namespace cv { class Mat; }
#endif

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rk_core {

/**
 * @brief Unified embedder (face recognition) interface for ModelRegistry.
 *
 * All embedder implementations (ArcFace, MobileFaceNet, LBPH)
 * SHALL implement this interface so they can be registered and created
 * through the ModelRegistry singleton.
 */
class Embedder {
public:
    virtual ~Embedder() = default;

    /**
     * @brief Load recognition model from the given path.
     * @param modelPath Path to the model file.
     * @param err Output error message on failure.
     * @return true if model loaded successfully.
     */
    virtual bool load(const std::string& modelPath, std::string& err) = 0;

    /**
     * @brief Compute embedding from an aligned face BGR image.
     * @param alignedFaceBgr 112x112 aligned face image (BGR).
     * @param err Output error message on failure.
     * @return Embedding vector, or nullopt on failure.
     */
    virtual std::optional<std::vector<float>> embed(const cv::Mat& alignedFaceBgr, std::string& err) = 0;

    /**
     * @brief Returns the fixed dimension of the embedding vector.
     *        Returns 0 if the embedder does not produce fixed-dim vectors.
     */
    virtual int embeddingDim() const = 0;

    /**
     * @brief Returns a human-readable name for this embedder instance.
     */
    virtual const char* name() const = 0;

    /**
     * @brief Returns the API version of this embedder implementation.
     */
    virtual std::uint32_t apiVersion() const { return 1; }
};

} // namespace rk_core
