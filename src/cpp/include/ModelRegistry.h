#pragma once

#include "Embedder.h"
#include "FaceDetector.h"

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace rk_core {

/**
 * @brief Metadata entry for a registered model.
 *
 * Each model registered in ModelRegistry carries an entry with
 * display name, task type, usage notes, and recommendation tags
 * so that the Web UI or CLI can present meaningful choices.
 */
struct ModelEntry {
    std::string id;
    std::string displayName;
    std::string taskType;         // "detect", "recognize", "detect_recognize"
    std::string notes;            // 适用场景说明（中文）
    std::string recommendedFor;   // "high_accuracy", "balanced", "high_speed"
    int priority = 0;             // 优先级（越小越优先）
};

/**
 * @brief Singleton model registry and factory.
 *
 * Usage:
 *   auto& reg = ModelRegistry::instance();
 *   reg.registerDetector("yolo_face", factory_fn, entry);
 *   auto det = reg.createDetector("yolo_face");
 *   auto all = reg.listAll();
 */
class ModelRegistry {
public:
    using DetectorFactory = std::function<std::unique_ptr<FaceDetector>()>;
    using EmbedderFactory = std::function<std::unique_ptr<Embedder>()>;

    static ModelRegistry& instance();

    /**
     * @brief Register built-in models on first call (idempotent).
     * Call once during application startup.
     */
    static void ensureBuiltinRegistered();

    // --- Registration ---
    void registerDetector(const std::string& id, DetectorFactory factory, const ModelEntry& entry);
    void registerEmbedder(const std::string& id, EmbedderFactory factory, const ModelEntry& entry);

    // --- Factory ---
    std::unique_ptr<FaceDetector> createDetector(const std::string& id, std::string* err = nullptr);
    std::unique_ptr<Embedder>     createEmbedder(const std::string& id, std::string* err = nullptr);

    // --- Query ---
    const ModelEntry* getEntry(const std::string& id) const;
    std::vector<ModelEntry> listAll() const;
    std::vector<ModelEntry> listByTask(const std::string& taskType) const;

    // --- Reload (re-register with new factory) ---
    bool reloadDetector(const std::string& id, DetectorFactory factory);
    bool reloadEmbedder(const std::string& id, EmbedderFactory factory);

private:
    ModelRegistry() = default;
    ModelRegistry(const ModelRegistry&) = delete;
    ModelRegistry& operator=(const ModelRegistry&) = delete;

    struct DetectorSlot {
        DetectorFactory factory;
        ModelEntry entry;
    };
    struct EmbedderSlot {
        EmbedderFactory factory;
        ModelEntry entry;
    };
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, DetectorSlot> detectors_;
    std::unordered_map<std::string, EmbedderSlot> embedders_;
};

} // namespace rk_core
