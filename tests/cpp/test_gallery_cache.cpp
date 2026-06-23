#include "FaceInferStages.h"
#include "FaceInferencePipeline.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

bool test_gallery_cache_empty_dir() {
    FaceInferRequest req;
    FaceInferContext ctx;
    FaceInferMetrics m;

    // Empty gallery dir should succeed with no entries
    auto s = FaceInferStages::loadGallery(req, ctx);
    if (!s.ok) { std::fprintf(stderr, "expected ok for empty dir\n"); return false; }
    if (!ctx.galleryEntries.empty()) { std::fprintf(stderr, "expected no entries\n"); return false; }
    return true;
}

bool test_gallery_cache_repeated_call() {
    // Create a temp gallery dir with a dummy file
    auto tmp = std::filesystem::temp_directory_path();
    auto dir = tmp / ("rk_test_gallery_" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    auto f = dir / "person1.bin";
    {
        std::ofstream ofs(f.string(), std::ios::binary);
        std::uint8_t hdr[24] = {0};
        ofs.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    }

    FaceInferRequest req;
    req.galleryDir = dir.string();
    FaceInferContext ctx;
    FaceInferMetrics m;

    auto s1 = FaceInferStages::loadGallery(req, ctx);
    if (!s1.ok) { std::fprintf(stderr, "first call failed\n"); return false; }

    auto s2 = FaceInferStages::loadGallery(req, ctx);
    if (!s2.ok) { std::fprintf(stderr, "second call failed\n"); return false; }
    // If cache works, second call returns the same entries without re-reading.
    // We can't easily verify I/O avoidance, but at minimum verify no crash.

    std::filesystem::remove_all(dir);
    return true;
}
