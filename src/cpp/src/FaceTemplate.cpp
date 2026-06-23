#include "FaceTemplate.h"

#include <cstring>

namespace {

static void putU32BE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 0) & 0xFF));
}

static void putU16BE(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 0) & 0xFF));
}

static bool readU32BE(const std::vector<std::uint8_t>& in, std::size_t& i, std::uint32_t& out) {
    if (i + 4 > in.size()) return false;
    out = (static_cast<std::uint32_t>(in[i]) << 24) | (static_cast<std::uint32_t>(in[i + 1]) << 16) |
          (static_cast<std::uint32_t>(in[i + 2]) << 8) | (static_cast<std::uint32_t>(in[i + 3]) << 0);
    i += 4;
    return true;
}

static bool readU16BE(const std::vector<std::uint8_t>& in, std::size_t& i, std::uint16_t& out) {
    if (i + 2 > in.size()) return false;
    out = static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[i]) << 8) | static_cast<std::uint16_t>(in[i + 1]));
    i += 2;
    return true;
}

static void putF32(std::vector<std::uint8_t>& out, float f, FaceTemplateHeader::Endian e) {
    std::uint32_t bits = 0;
    static_assert(sizeof(float) == 4, "float must be 32-bit");
    std::memcpy(&bits, &f, sizeof(bits));
    if (e == FaceTemplateHeader::Endian::Little) {
        out.push_back(static_cast<std::uint8_t>((bits >> 0) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((bits >> 24) & 0xFF));
    } else {
        out.push_back(static_cast<std::uint8_t>((bits >> 24) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((bits >> 0) & 0xFF));
    }
}

static bool readF32(const std::vector<std::uint8_t>& in, std::size_t& i, float& out, FaceTemplateHeader::Endian e) {
    if (i + 4 > in.size()) return false;
    std::uint32_t bits = 0;
    if (e == FaceTemplateHeader::Endian::Little) {
        bits = (static_cast<std::uint32_t>(in[i]) << 0) | (static_cast<std::uint32_t>(in[i + 1]) << 8) |
               (static_cast<std::uint32_t>(in[i + 2]) << 16) | (static_cast<std::uint32_t>(in[i + 3]) << 24);
    } else {
        bits = (static_cast<std::uint32_t>(in[i]) << 24) | (static_cast<std::uint32_t>(in[i + 1]) << 16) |
               (static_cast<std::uint32_t>(in[i + 2]) << 8) | (static_cast<std::uint32_t>(in[i + 3]) << 0);
    }
    i += 4;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

static bool isLittleEndianHost() {
    std::uint32_t x = 1;
    return *(reinterpret_cast<std::uint8_t*>(&x)) == 1;
}

}  // namespace

std::vector<std::uint8_t> serializeFaceTemplate(const FaceTemplate& t) {
    FaceTemplateHeader h = t.header;
    h.payloadBytes = static_cast<std::uint32_t>(t.embedding.size() * 4);

    std::vector<std::uint8_t> out;
    out.reserve(24 + static_cast<std::size_t>(h.payloadBytes));

    putU32BE(out, FaceTemplateHeader::kMagic);
    out.push_back(h.version);
    out.push_back(static_cast<std::uint8_t>(h.dtype));
    out.push_back(static_cast<std::uint8_t>(h.norm));
    out.push_back(static_cast<std::uint8_t>(h.floatEndian));
    out.push_back(0);
    putU16BE(out, h.dim);
    putU32BE(out, h.modelVersion);
    putU32BE(out, h.embedderApiVersion);
    putU32BE(out, h.payloadBytes);

    const auto endian = h.floatEndian;
    for (float v : t.embedding) putF32(out, v, endian);
    return out;
}

std::optional<FaceTemplate> deserializeFaceTemplate(const std::vector<std::uint8_t>& bytes, std::string* err) {
    FaceTemplate t;
    std::size_t i = 0;

    std::uint32_t magic = 0;
    if (!readU32BE(bytes, i, magic) || magic != FaceTemplateHeader::kMagic) {
        if (err) *err = "FaceTemplate: magic 不匹配";
        return std::nullopt;
    }

    if (i + 5 + 2 + 4 + 4 + 4 > bytes.size()) {
        if (err) *err = "FaceTemplate: 数据长度不足";
        return std::nullopt;
    }

    t.header.version = bytes[i++];
    if (t.header.version != FaceTemplateHeader::kVersion) {
        if (err) *err = "FaceTemplate: 不支持的 version";
        return std::nullopt;
    }

    t.header.dtype = static_cast<FaceTemplateHeader::DType>(bytes[i++]);
    t.header.norm = static_cast<FaceTemplateHeader::Norm>(bytes[i++]);
    t.header.floatEndian = static_cast<FaceTemplateHeader::Endian>(bytes[i++]);
    i += 1;

    if (t.header.dtype != FaceTemplateHeader::DType::Float32) {
        if (err) *err = "FaceTemplate: 不支持的 dtype";
        return std::nullopt;
    }
    if (t.header.floatEndian != FaceTemplateHeader::Endian::Little && t.header.floatEndian != FaceTemplateHeader::Endian::Big) {
        if (err) *err = "FaceTemplate: 不支持的 float_endian";
        return std::nullopt;
    }

    if (!readU16BE(bytes, i, t.header.dim)) {
        if (err) *err = "FaceTemplate: dim 读取失败";
        return std::nullopt;
    }
    if (!readU32BE(bytes, i, t.header.modelVersion) || !readU32BE(bytes, i, t.header.embedderApiVersion) ||
        !readU32BE(bytes, i, t.header.payloadBytes)) {
        if (err) *err = "FaceTemplate: header 读取失败";
        return std::nullopt;
    }

    if (t.header.dim != 512) {
        if (err) *err = "FaceTemplate: dim 非 512";
        return std::nullopt;
    }

    const std::uint32_t expectedBytes = static_cast<std::uint32_t>(t.header.dim) * 4U;
    if (t.header.payloadBytes != expectedBytes) {
        if (err) *err = "FaceTemplate: payloadBytes 不匹配";
        return std::nullopt;
    }
    if (i + static_cast<std::size_t>(t.header.payloadBytes) > bytes.size()) {
        if (err) *err = "FaceTemplate: payload 数据长度不足";
        return std::nullopt;
    }

    t.embedding.resize(t.header.dim);
    const auto endian = t.header.floatEndian;
    for (std::size_t k = 0; k < t.embedding.size(); k++) {
        float v = 0.0f;
        if (!readF32(bytes, i, v, endian)) {
            if (err) *err = "FaceTemplate: float32 读取失败";
            return std::nullopt;
        }
        t.embedding[k] = v;
    }

    if (t.header.floatEndian != (isLittleEndianHost() ? FaceTemplateHeader::Endian::Little : FaceTemplateHeader::Endian::Big)) {
        t.header.floatEndian = isLittleEndianHost() ? FaceTemplateHeader::Endian::Little : FaceTemplateHeader::Endian::Big;
    }

    return t;
}

