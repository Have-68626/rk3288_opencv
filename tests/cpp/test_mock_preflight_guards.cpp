#include "VideoManager.h"

using namespace rk_core;

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path findFixture(const std::string& name) {
    const auto cwd = std::filesystem::current_path();
    const std::vector<std::filesystem::path> candidates = {
        cwd / "tests" / "fixtures" / "mock" / name,
        cwd.parent_path() / "tests" / "fixtures" / "mock" / name
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && !ec) {
            return p;
        }
    }
    return {};
}

std::filesystem::path makeTempDir(const std::string& suffix) {
    const auto base = std::filesystem::temp_directory_path() / "rk3288_opencv_mock_preflight_tests";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = base / (suffix + "_" + std::to_string(stamp));
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// 手工构造 24-bit BMP：头部声明 width×height，仅写入 dataLen 字节数据区。
// ihdrSize=40（BITMAPINFOHEADER，32 位宽高）；ihdrSize=12（OS/2 core-header，16 位宽高）。
static bool writeBmp(const std::filesystem::path& p, int width, int height, int dataLen, int ihdrSize = 40) {
    std::ofstream out(p, std::ios::binary);
    if (!out.is_open()) return false;
    const std::int32_t dataOffset = 14 + ihdrSize;
    const std::int32_t fileSize = dataOffset + dataLen;
    auto put32 = [&out](std::int32_t v) {
        out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    };
    auto put16 = [&out](std::int16_t v) {
        out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    };
    out.put('B'); out.put('M');
    put32(fileSize);
    put32(0);          // reserved
    put32(dataOffset); // pixel data offset
    put32(ihdrSize);   // DIB header size
    if (ihdrSize == 12) {
        // OS/2 core-header：biWidth/biHeight 为 16 位
        put16(static_cast<std::int16_t>(width));
        put16(static_cast<std::int16_t>(height));
        put16(1);          // planes
        put16(24);         // bpp
    } else {
        put32(width);      // declared width
        put32(height);     // declared height
        put16(1);          // planes
        put16(24);         // bpp
        put32(0);          // compression (BI_RGB)
        put32(0);          // image size
        put32(0); put32(0); put32(0); put32(0); // xppm/yppm/clrUsed/clrImportant
    }
    for (int i = 0; i < dataLen; i++) out.put(0);
    return static_cast<bool>(out);
}

// 构造声明巨大尺寸的 PNG（签名 + IHDR 宽/高，无需合法 CRC），实际文件极小。
static bool writeHugeDimPng(const std::filesystem::path& p, std::uint32_t w, std::uint32_t h) {
    std::ofstream out(p, std::ios::binary);
    if (!out.is_open()) return false;
    const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.write(reinterpret_cast<const char*>(sig), 8);
    auto put32be = [&out](std::uint32_t v) {
        const unsigned char b[4] = {static_cast<unsigned char>((v >> 24) & 0xFF),
                                    static_cast<unsigned char>((v >> 16) & 0xFF),
                                    static_cast<unsigned char>((v >> 8) & 0xFF),
                                    static_cast<unsigned char>(v & 0xFF)};
        out.write(reinterpret_cast<const char*>(b), 4);
    };
    put32be(13);   // IHDR chunk length
    out.write("IHDR", 4);
    put32be(w);    // declared width
    put32be(h);    // declared height
    for (int i = 0; i < 5; i++) out.put(0);  // bit depth/color type/compression/filter/interlace
    return static_cast<bool>(out);
}

// 构造声明巨大尺寸的有损 WebP（VP8 关键帧头），无有效压缩数据。
static bool writeHugeDimWebp(const std::filesystem::path& p, int width, int height) {
    std::ofstream out(p, std::ios::binary);
    if (!out.is_open()) return false;
    auto put32le = [&out](std::uint32_t v) {
        const unsigned char b[4] = {static_cast<unsigned char>(v & 0xFF),
                                    static_cast<unsigned char>((v >> 8) & 0xFF),
                                    static_cast<unsigned char>((v >> 16) & 0xFF),
                                    static_cast<unsigned char>((v >> 24) & 0xFF)};
        out.write(reinterpret_cast<const char*>(b), 4);
    };
    const std::uint32_t chunkSize = 32;  // 帧标签(3) + 起始码(3) + 宽高(4) + 占位
    const std::uint32_t fileSize = 20 + chunkSize;
    out.write("RIFF", 4);
    put32le(fileSize - 8);
    out.write("WEBP", 4);
    out.write("VP8 ", 4);
    put32le(chunkSize);
    out.put(0x10); out.put(0x00); out.put(0x00);  // 关键帧标签（bit4=show_frame）
    out.put(0x9D); out.put(0x01); out.put(0x2A);  // 起始码
    out.put(static_cast<char>(width & 0xFF));
    out.put(static_cast<char>((width >> 8) & 0xFF));
    out.put(static_cast<char>(height & 0xFF));
    out.put(static_cast<char>((height >> 8) & 0xFF));
    for (int i = 0; i < 16; i++) out.put(0);
    return static_cast<bool>(out);
}

// 构造声明巨大尺寸的 JPEG：SOI(FF D8) + SOF0 段声明 height×width（均 2 字节大端），
// 后续 component 字节用占位填充，实际文件极小。SOF0 布局（i 为标记起点）：
// FF C0 <segLen BE=17> 08 <height 2B BE> <width 2B BE> 03 <3×component>，
// 与 parseJpegDims 读取偏移一致（height 在 i+5..i+6、width 在 i+7..i+8）。
static bool writeHugeDimJpeg(const std::filesystem::path& p, int width, int height) {
    std::ofstream out(p, std::ios::binary);
    if (!out.is_open()) return false;
    auto put16be = [&out](int v) {
        const unsigned char b[2] = {static_cast<unsigned char>((v >> 8) & 0xFF),
                                    static_cast<unsigned char>(v & 0xFF)};
        out.write(reinterpret_cast<const char*>(b), 2);
    };
    out.put(0xFF); out.put(0xD8);  // SOI
    out.put(0xFF); out.put(0xC0);  // SOF0 标记
    put16be(17);                   // 段长（含 2 长度字节 + 15 载荷字节）
    out.put(8);                    // precision（每分量 8 bit）
    put16be(height);               // 声明高度
    put16be(width);                // 声明宽度
    out.put(3);                    // component 数（YCbCr）
    for (int i = 0; i < 9; i++) out.put(0);  // 3×component 描述符占位
    return static_cast<bool>(out);
}

}  // namespace

bool test_mock_preflight_rejects_invalid_magic() {
    const auto path = findFixture("corrupt_magic.jpg");
    if (path.empty()) return false;

    std::string reason;
    const bool ok = VideoManager::preflightMockInput(path.string(), &reason);
    if (ok) return false;
    if (reason != "MOCK_MAGIC_INVALID") return false;
    return true;
}

bool test_mock_preflight_rejects_incomplete_file() {
    const auto path = findFixture("incomplete.jpg");
    if (path.empty()) return false;

    std::string reason;
    const bool ok = VideoManager::preflightMockInput(path.string(), &reason);
    if (ok) return false;
    if (reason != "MOCK_FILE_INCOMPLETE") return false;
    return true;
}

bool test_mock_preflight_rejects_oversize_image() {
    const auto dir = makeTempDir("oversize");
    const auto path = dir / "oversize.jpg";
    {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) return false;
        out.seekp(static_cast<std::streamoff>(50LL * 1024LL * 1024LL));
        out.put('\0');
    }

    std::string reason;
    const bool ok = VideoManager::preflightMockInput(path.string(), &reason);
    if (ok) return false;
    if (reason != "MOCK_OVERSIZE") return false;
    return true;
}

bool test_mock_preflight_rejects_huge_declared_dimension() {
    // 恶意输入：文件极小，但 BMP 头部声明 65536×65536 像素。
    // 若未在解码前校验声明尺寸，imdecode 会按头部分配 ~12GB 矩阵。
    const auto dir = makeTempDir("huge_dim");
    const auto path = dir / "huge.bmp";
    if (!writeBmp(path, 65536, 65536, 64)) return false;

    std::string reason;
    const bool ok = VideoManager::preflightMockInput(path.string(), &reason);
    if (ok) return false;
    if (reason != "MOCK_IMAGE_DIMENSION_OVERFLOW") return false;
    return true;
}

bool test_mock_preflight_accepts_normal_bmp() {
    const auto dir = makeTempDir("normal_bmp");
    const auto path = dir / "ok.bmp";
    if (!writeBmp(path, 64, 48, 64 * 48 * 3)) return false;

    std::string reason;
    return VideoManager::preflightMockInput(path.string(), &reason);
}

bool test_mock_preflight_accepts_normal_os2_bmp() {
    // OS/2 core-header（biSize=12）合法 BMP：宽/高为 16 位字段。
    // 若按 32 位布局误读（planes|bpp<<16 落在宽高字段），正常小图会被误拒
    // MOCK_IMAGE_DIMENSION_OVERFLOW；此处验证正常 OS/2 BMP 不被误拒。
    const auto dir = makeTempDir("normal_os2_bmp");
    const auto path = dir / "ok_os2.bmp";
    if (!writeBmp(path, 64, 48, 64 * 48 * 3, 12)) return false;

    std::string reason;
    return VideoManager::preflightMockInput(path.string(), &reason);
}

bool test_mock_preflight_rejects_huge_os2_bmp_dimension() {
    // OS/2 core-header（biSize=12）的 16 位宽/高路径仍需防 OOM：65535 声明仍 >4096 上界。
    const auto dir = makeTempDir("huge_os2_bmp");
    const auto path = dir / "huge_os2.bmp";
    if (!writeBmp(path, 65535, 65535, 64, 12)) return false;

    std::string reason;
    const bool ok = VideoManager::preflightMockInput(path.string(), &reason);
    if (ok) return false;
    if (reason != "MOCK_IMAGE_DIMENSION_OVERFLOW") return false;
    return true;
}

bool test_mock_preflight_rejects_huge_png_dimension() {
    const auto dir = makeTempDir("huge_png");
    const auto path = dir / "huge.png";
    if (!writeHugeDimPng(path, 20000, 20000)) return false;

    std::string reason;
    const bool ok = VideoManager::preflightMockInput(path.string(), &reason);
    if (ok) return false;
    if (reason != "MOCK_IMAGE_DIMENSION_OVERFLOW") return false;
    return true;
}

bool test_mock_preflight_accepts_real_png() {
    const auto dir = makeTempDir("real_png");
    const auto path = dir / "ok.png";
    cv::Mat img(96, 128, CV_8UC3, cv::Scalar(40, 60, 80));
    if (!cv::imwrite(path.string(), img)) return false;

    std::string reason;
    return VideoManager::preflightMockInput(path.string(), &reason);
}

bool test_mock_preflight_rejects_huge_webp_dimension() {
    const auto dir = makeTempDir("huge_webp");
    const auto path = dir / "huge.webp";
    if (!writeHugeDimWebp(path, 5000, 5000)) return false;

    std::string reason;
    const bool ok = VideoManager::preflightMockInput(path.string(), &reason);
    if (ok) return false;
    if (reason != "MOCK_IMAGE_DIMENSION_OVERFLOW") return false;
    return true;
}

bool test_mock_preflight_accepts_real_webp() {
    const auto dir = makeTempDir("real_webp");
    const auto path = dir / "ok.webp";
    cv::Mat img(64, 80, CV_8UC3, cv::Scalar(30, 50, 70));
    if (!cv::imwrite(path.string(), img)) return false;

    std::string reason;
    return VideoManager::preflightMockInput(path.string(), &reason);
}

bool test_mock_preflight_rejects_huge_jpeg_dimension() {
    // 恶意输入：文件极小，但 JPEG SOF0 段声明 50000×50000 像素。
    // 若未在解码前校验 SOF 声明尺寸，imdecode 会按头部分配 ~7.5GB 矩阵。
    const auto dir = makeTempDir("huge_jpeg");
    const auto path = dir / "huge.jpg";
    if (!writeHugeDimJpeg(path, 50000, 50000)) return false;

    std::string reason;
    const bool ok = VideoManager::preflightMockInput(path.string(), &reason);
    if (ok) return false;
    if (reason != "MOCK_IMAGE_DIMENSION_OVERFLOW") return false;
    return true;
}

bool test_mock_preflight_accepts_real_jpeg() {
    const auto dir = makeTempDir("real_jpeg");
    const auto path = dir / "ok.jpg";
    cv::Mat img(64, 48, CV_8UC3, cv::Scalar(50, 70, 90));
    if (!cv::imwrite(path.string(), img)) return false;

    std::string reason;
    return VideoManager::preflightMockInput(path.string(), &reason);
}
