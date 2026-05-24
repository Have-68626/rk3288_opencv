#include "rk_win/FaceDatabase.h"

#include <iostream>
#include <chrono>
#include <vector>
#include <filesystem>
#include <random>

using namespace rk_win;

int main() {
    FaceDatabase db;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Populate database with dummy data
    for (int i = 0; i < 100; i++) {
        std::vector<float> mean(512);
        for (int j = 0; j < 512; j++) {
            mean[j] = dist(rng);
        }
        db.updateMean("person_" + std::to_string(i), mean);
    }

    const std::filesystem::path testPath = std::filesystem::temp_directory_path() / "test_face_database_perf.xml";

    // Timing save()
    int iterations = 1000;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        db.save(testPath);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto saveDuration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Timing load()
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        FaceDatabase loadDb;
        loadDb.load(testPath);
    }
    end = std::chrono::high_resolution_clock::now();
    auto loadDuration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Validate correctness
    FaceDatabase loadDb;
    loadDb.load(testPath);
    bool valid = true;
    if (loadDb.persons().size() != db.persons().size()) valid = false;
    else {
        for (const auto& kv : db.persons()) {
            if (!loadDb.hasPerson(kv.first)) {
                valid = false;
                break;
            }
            const auto& origE = kv.second;
            const auto& loadE = loadDb.persons().at(kv.first);
            if (origE.count != loadE.count || origE.mean.size() != loadE.mean.size()) {
                valid = false;
                break;
            }
            for (size_t i = 0; i < origE.mean.size(); i++) {
                // Approximate floating-point match
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

    std::filesystem::remove(testPath);

    return valid ? 0 : 1;
}
