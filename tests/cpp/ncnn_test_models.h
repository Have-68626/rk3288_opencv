#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace rk_test {

// 常量图走真实 ncnn forward，奇数特征图可暴露通道 cstep 补齐后的复制错位。
inline bool writeConstantScrfdModel(const std::filesystem::path& dir, int inputSide) {
    std::filesystem::create_directories(dir);
    std::ofstream param(dir / "scrfd.param");
    std::ofstream bin(dir / "scrfd.bin", std::ios::binary);
    param << "7767517\n10 10\nInput input 0 1 input.1\n";
    const char* prefixes[] = {"s", "b", "k"};
    const int channels[] = {2, 8, 20};
    for (int kind = 0; kind < 3; ++kind) {
        for (int level = 0; level < 3; ++level) {
            const int stride = 8 << level;
            const int side = inputSide / stride;
            const int grid = side * side;
            const std::string name = std::string(prefixes[kind]) + std::to_string(stride);
            param << "MemoryData " << name << " 0 1 " << name
                  << " 0=" << side << " 1=" << side << " 2=" << channels[kind] << '\n';
            std::vector<float> data(static_cast<std::size_t>(grid) * channels[kind],
                                    kind == 1 ? 1.0f : 0.0f);
            if (kind == 0 && level == 2) data[grid + (side / 2) * side + side / 2] = 0.9f;
            bin.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size() * sizeof(float)));
        }
    }
    return static_cast<bool>(param) && static_cast<bool>(bin);
}

inline bool writeEmbeddingModel(const std::filesystem::path& dir, bool int8, bool mixed = false) {
    std::filesystem::create_directories(dir);
    std::ofstream param(dir / "embedding.param");
    std::ofstream bin(dir / "embedding.bin", std::ios::binary);
    param << "7767517\n" << (mixed ? 4 : 3) << ' ' << (mixed ? 4 : 3)
          << "\nInput input 0 1 in0\nPooling pool 1 1 in0 pooled 0=1 4=1\n";
    const auto writeFloat = [&](float value) {
        bin.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    const auto writeTag = [&](unsigned int value) {
        bin.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    if (mixed) {
        param << "InnerProduct fp32 1 1 pooled mixed 0=3 2=9\n";
        writeTag(0);
        for (int i = 0; i < 9; ++i) writeFloat(i % 4 == 0 ? 1.0f : 0.0f);
    }
    param << "InnerProduct embedding 1 1 " << (mixed ? "mixed" : "pooled")
          << " out0 0=512 2=1536" << (int8 ? " 8=2" : "") << '\n';
    if (int8) {
        writeTag(0x000D4B38);
        std::vector<char> weights(1536, 1);
        bin.write(weights.data(), static_cast<std::streamsize>(weights.size()));
        for (int i = 0; i < 512; ++i) writeFloat(1.0f);
        writeFloat(128.0f);
    } else {
        writeTag(0);
        for (int i = 0; i < 1536; ++i) writeFloat(1.0f);
    }
    return static_cast<bool>(param) && static_cast<bool>(bin);
}

}  // namespace rk_test
