#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct FaceTemplateHeader {
    static constexpr std::uint32_t kMagic = 0x4654504C;
    static constexpr std::uint8_t kVersion = 1;

    enum class DType : std::uint8_t { Float32 = 1 };
    enum class Norm : std::uint8_t { None = 0, L2 = 1 };
    enum class Endian : std::uint8_t { Little = 1, Big = 2 };

    std::uint8_t version = kVersion;
    DType dtype = DType::Float32;
    Norm norm = Norm::L2;
    Endian floatEndian = Endian::Little;
    std::uint16_t dim = 512;
    std::uint32_t modelVersion = 1;
    std::uint32_t embedderApiVersion = 1;
    std::uint32_t payloadBytes = 512 * 4;
};

struct FaceTemplate {
    FaceTemplateHeader header{};
    std::vector<float> embedding;
};

std::vector<std::uint8_t> serializeFaceTemplate(const FaceTemplate& t);
std::optional<FaceTemplate> deserializeFaceTemplate(const std::vector<std::uint8_t>& bytes, std::string* err);

