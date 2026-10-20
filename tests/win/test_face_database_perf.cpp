#include "rk_win/FaceDatabase.h"

#include <iostream>
#include <chrono>
#include <vector>
#include <filesystem>
#include <random>
#include <process.h>
#include <algorithm>
#include <cmath>
#include <numeric>

using namespace rk_win;

int main(int argc, char** argv) {
    int iterations = 1000, warmup = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (i + 1 >= argc) return 2;
        if (option == "--iters") iterations = std::stoi(argv[++i]);
        else if (option == "--warmup") warmup = std::stoi(argv[++i]);
        else return 2;
    }
    if (iterations <= 0 || warmup < 0) return 2;
    FaceDatabase db;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // 固定种子只生成合成向量，不读取真实人脸库。
    for (int i = 0; i < 100; i++) {
        std::vector<float> mean(512);
        for (int j = 0; j < 512; j++) {
            mean[j] = dist(rng);
        }
        db.updateMean("person_" + std::to_string(i), mean);
    }

    // 临时库文件路径唯一化：进程 ID + 时间戳后缀，避免并发运行碰撞（不同进程各自独立路径，互不覆盖）。
    const std::filesystem::path testPath = std::filesystem::temp_directory_path() /
        ("test_face_database_perf_" + std::to_string(::_getpid()) + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".xml");

    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::remove(FaceDatabase::keyPathFor(path), error);
        }
    } cleanup{testPath};
    auto measure = [&](auto operation, std::vector<double>& samples) {
        for (int i = 0; i < warmup; ++i) if (!operation()) return false;
        for (int i = 0; i < iterations; ++i) {
            const auto start = std::chrono::steady_clock::now();
            if (!operation()) return false;
            samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        return true;
    };
    std::vector<double> saveSamples, loadSamples;
    if (!measure([&] { return db.save(testPath); }, saveSamples) ||
        !measure([&] { FaceDatabase loaded; return loaded.load(testPath); }, loadSamples)) {
        std::cerr << "TEST_RESULT: FAIL operation_failed\n";
        return 1;
    }
    const double saveDuration = std::accumulate(saveSamples.begin(), saveSamples.end(), 0.0);
    const double loadDuration = std::accumulate(loadSamples.begin(), loadSamples.end(), 0.0);
    std::sort(saveSamples.begin(), saveSamples.end());
    std::sort(loadSamples.begin(), loadSamples.end());
    const auto percentile = [](const std::vector<double>& samples, double p) {
        return samples[static_cast<std::size_t>(std::ceil(p * samples.size())) - 1];
    };

    FaceDatabase loadDb;
    loadDb.load(testPath);
    // persons() 按值返回整表：循环外各取一次副本，避免每次迭代整表拷贝，
    // 也避免 const auto& 绑定到 .at() 返回的临时对象元素（悬垂引用，依赖生命周期延伸）。
    const auto origPersons = db.persons();
    const auto loadedPersons = loadDb.persons();
    bool valid = true;
    if (loadedPersons.size() != origPersons.size()) valid = false;
    else {
        for (const auto& kv : origPersons) {
            const auto it = loadedPersons.find(kv.first);
            if (it == loadedPersons.end()) {
                valid = false;
                break;
            }
            const auto& origE = kv.second;
            const auto& loadE = it->second;
            if (origE.count != loadE.count || origE.mean.size() != loadE.mean.size()) {
                valid = false;
                break;
            }
            for (size_t i = 0; i < origE.mean.size(); i++) {
                if (std::abs(origE.mean[i] - loadE.mean[i]) > 1e-5f) {
                    valid = false;
                    break;
                }
            }
        }
    }

    std::cout << "TEST_RESULT: " << (valid ? "PASS" : "FAIL") << "\n";
    std::cout << "SAVE_TIME_MS: " << saveDuration << "\n";
    std::cout << "LOAD_TIME_MS: " << loadDuration << "\n";
    std::cout << "BENCH_SCOPE warmup_iters=" << warmup << " measured_iters=" << iterations
              << " includes_cold_start=" << (warmup == 0 ? "true" : "false") << "\n";
    std::cout << "SAVE_P50_MS: " << percentile(saveSamples, 0.50) << "\n";
    std::cout << "SAVE_P95_MS: " << percentile(saveSamples, 0.95) << "\n";
    std::cout << "LOAD_P50_MS: " << percentile(loadSamples, 0.50) << "\n";
    std::cout << "LOAD_P95_MS: " << percentile(loadSamples, 0.95) << "\n";

    return valid ? 0 : 1;
}
