#include "rk_win/FaceRecognizer.h"
#include "rk_win/StructuredLogger.h"
#include "rk_win/WinConfig.h"

#include <opencv2/imgcodecs.hpp>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace rk_win {
namespace {

struct Args {
    std::filesystem::path inputDir = "tests/test_set01";
    std::filesystem::path cascadePath = "tests/data/lbpcascade_frontalface.xml";
    std::filesystem::path dbPath = "storage/win_bench_db.yml";
    int minFaceSizePx = 60;
    double threshold = 55.0;
    int iters = 200;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        auto nextPath = [&](std::filesystem::path& v) {
            if (i + 1 < argc) v = argv[++i];
        };
        auto nextInt = [&](int& v) {
            if (i + 1 < argc) v = std::stoi(argv[++i]);
        };
        auto nextDouble = [&](double& v) {
            if (i + 1 < argc) v = std::stod(argv[++i]);
        };

        if (k == "--input") nextPath(a.inputDir);
        else if (k == "--cascade") nextPath(a.cascadePath);
        else if (k == "--db") nextPath(a.dbPath);
        else if (k == "--min_face") nextInt(a.minFaceSizePx);
        else if (k == "--threshold") nextDouble(a.threshold);
        else if (k == "--iters") nextInt(a.iters);
    }
    return a;
}

bool isImageFile(const std::filesystem::path& p) {
    auto e = p.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".bmp";
}

std::uint64_t rssBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<std::uint64_t>(pmc.WorkingSetSize);
    }
    return 0;
#else
    return 0;
#endif
}

}  // namespace
}  // namespace rk_win

int main(int argc, char** argv) {
    using namespace rk_win;
    const auto args = parseArgs(argc, argv);

    const auto inputDir = resolvePathFromExeDir(args.inputDir);
    const auto cascadePath = resolvePathFromExeDir(args.cascadePath);
    const auto dbPath = resolvePathFromExeDir(args.dbPath);

    std::vector<std::filesystem::path> images;
    for (const auto& e : std::filesystem::directory_iterator(inputDir)) {
        if (!e.is_regular_file()) continue;
        if (!isImageFile(e.path())) continue;
        images.push_back(e.path());
    }
    if (images.empty()) {
        std::cerr << "BENCH_ERROR no_images input=" << inputDir.string() << std::endl;
        return 2;
    }

    FaceRecognizer rec;
    if (!rec.initialize(cascadePath.string(), dbPath, args.minFaceSizePx, args.threshold)) {
        std::cerr << "BENCH_ERROR init_failed cascade=" << cascadePath.string() << std::endl;
        return 3;
    }

    std::vector<cv::Mat> mats;
    mats.reserve(images.size());
    for (const auto& p : images) {
        cv::Mat img = cv::imread(p.string(), cv::IMREAD_COLOR);
        if (!img.empty()) mats.push_back(std::move(img));
    }
    if (mats.empty()) {
        std::cerr << "BENCH_ERROR image_load_failed" << std::endl;
        return 4;
    }

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    std::uint64_t frames = 0;
    std::uint64_t faces = 0;

    for (int it = 0; it < args.iters; it++) {
        for (const auto& img : mats) {
            const auto r = rec.identify(img);
            frames++;
            faces += static_cast<std::uint64_t>(r.size());
        }
    }
    const auto t1 = clock::now();
    const double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count();
    const double fps = (ms > 0.0) ? (1000.0 * static_cast<double>(frames) / ms) : 0.0;
    const double avgMs = (frames > 0) ? (ms / static_cast<double>(frames)) : 0.0;

    std::cout << "BENCH_SUMMARY"
              << " images=" << mats.size()
              << " iters=" << args.iters
              << " frames=" << frames
              << " faces=" << faces
              << " ms_total=" << ms
              << " fps=" << fps
              << " avg_ms=" << avgMs
              << " rss_bytes=" << rssBytes()
              << std::endl;

    return 0;
}

