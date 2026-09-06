#include "FaceRuntimeProfile.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace rk_core;

namespace {

std::string makeOutputs9() {
    return "\"outputs\":[\"out0\",\"out1\",\"out2\",\"out3\",\"out4\",\"out5\",\"out6\",\"out7\",\"out8\"]";
}

std::string validManifest() {
    return
        "{\"schemaVersion\":1,"
        "\"profileId\":\"rk3288_light\",\"displayName\":\"RK3288 Light\","
        "\"supportedAbis\":[\"armeabi-v7a\",\"arm64-v8a\"],"
        "\"detector\":{\"param\":{\"path\":\"scrfd/scrfd.param\"},\"bin\":{\"path\":\"scrfd/scrfd.bin\"},"
        " \"input\":\"in0\"," + makeOutputs9() + ",\"inputWidth\":640,\"inputHeight\":640},"
        "\"embedder\":{\"param\":{\"path\":\"arc/arc.param\"},\"bin\":{\"path\":\"arc/arc.bin\"},"
        " \"input\":\"in0\",\"output\":\"out0\",\"inputWidth\":112,\"inputHeight\":112,\"embeddingDim\":512},"
        "\"detectionConfidence\":0.55,\"nmsIou\":0.40,\"identitySimilarity\":0.45,"
        "\"modelVersion\":\"1\",\"licenseId\":\"apache-2.0\"}";
}

std::filesystem::path tempBase() {
    return std::filesystem::temp_directory_path() / "rk_face_profile_test";
}

class FaceRuntimeManifestTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::error_code ec;
        std::filesystem::remove_all(tempBase(), ec);
        std::filesystem::create_directories(tempBase() / "scrfd", ec);
        std::filesystem::create_directories(tempBase() / "arc", ec);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tempBase(), ec);
    }
    void writeFile(const std::string& rel, const std::string& content) {
        std::ofstream out(tempBase() / rel, std::ios::binary);
        out << content;
    }
};

}  // namespace

TEST_F(FaceRuntimeManifestTest, ParseValidManifest) {
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_TRUE(FaceRuntimeManifest::parseV1(validManifest(), "arm64-v8a", p, err));
    EXPECT_EQ(p.profileId, "rk3288_light");
    EXPECT_EQ(p.detectorInput, "in0");
    EXPECT_EQ(p.detectorOutputs.size(), 9u);
    EXPECT_EQ(p.embeddingDim, 512);
    EXPECT_FLOAT_EQ(p.nmsIou, 0.40f);
}

TEST_F(FaceRuntimeManifestTest, UnknownSchemaVersionRejected) {
    std::string bad = validManifest();
    // 首个 ":1," 即 schemaVersion:1, → 改为 :2,（其余字段均非数字 1 冒号）
    auto pos = bad.find(":1,");
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, 3, ":2,");
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("schema"), std::string::npos);
}

TEST_F(FaceRuntimeManifestTest, AbsolutePathRejected) {
    std::string bad = validManifest();
    auto pos = bad.find("\"scrfd/scrfd.param\"");
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, std::string("\"scrfd/scrfd.param\"").size(), "\"/etc/evil.param\"");
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("路径"), std::string::npos);
}

TEST_F(FaceRuntimeManifestTest, ParentPathRejected) {
    std::string bad = validManifest();
    auto pos = bad.find("\"scrfd/scrfd.param\"");
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, std::string("\"scrfd/scrfd.param\"").size(), "\"../leak.param\"");
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("路径"), std::string::npos);
}

TEST_F(FaceRuntimeManifestTest, AbiMismatchRejected) {
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(validManifest(), "x86_64", p, err));
    EXPECT_NE(err.find("ABI"), std::string::npos);
}

TEST_F(FaceRuntimeManifestTest, InvalidThresholdRejected) {
    std::string bad = validManifest();
    auto pos = bad.find("0.55");
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, 4, "1.5");
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("阈值"), std::string::npos);
}

TEST_F(FaceRuntimeManifestTest, DetectorOutputsWrongCountRejected) {
    std::string bad = validManifest();
    auto pos = bad.find(makeOutputs9());
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, makeOutputs9().size(),
                "\"outputs\":[\"out0\",\"out1\",\"out2\"]");  // 3 个而非 9
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("9"), std::string::npos);
}

TEST_F(FaceRuntimeManifestTest, LfsPointerFileRejected) {
    writeFile("scrfd/scrfd.param",
              "version https://git-lfs.github.com/spec/v1\noid sha256:abc\nsize 1\n");
    writeFile("scrfd/scrfd.bin", "bin");
    writeFile("arc/arc.param", "arc_param");
    writeFile("arc/arc.bin", "arc_bin");
    FaceRuntimeProfile p;
    std::string err;
    ASSERT_TRUE(FaceRuntimeManifest::parseV1(validManifest(), "arm64-v8a", p, err));
    EXPECT_FALSE(FaceRuntimeManifest::verifyFiles(p, tempBase().string(), err));
    EXPECT_NE(err.find("LFS"), std::string::npos);
}

TEST_F(FaceRuntimeManifestTest, MissingFileRejected) {
    writeFile("scrfd/scrfd.bin", "bin");
    writeFile("arc/arc.param", "arc_param");
    writeFile("arc/arc.bin", "arc_bin");
    // scrfd.param 不存在
    FaceRuntimeProfile p;
    std::string err;
    ASSERT_TRUE(FaceRuntimeManifest::parseV1(validManifest(), "arm64-v8a", p, err));
    EXPECT_FALSE(FaceRuntimeManifest::verifyFiles(p, tempBase().string(), err));
}

TEST_F(FaceRuntimeManifestTest, HashMismatchRejected) {
    writeFile("scrfd/scrfd.param", "X");
    writeFile("scrfd/scrfd.bin", "Y");
    writeFile("arc/arc.param", "Z");
    writeFile("arc/arc.bin", "W");
    std::string manifest = validManifest();
    // 注入 sha256（对 "X" 内容错误）→ 校验失败
    std::string shaHex(64, '0');
    const std::string target = "\"scrfd/scrfd.param\"";
    auto pos = manifest.find(target);
    ASSERT_NE(pos, std::string::npos);
    // 在闭合引号之后插入 ",\"sha256\":\"...\""（事务形成合法字段）
    manifest.insert(pos + target.size(), ",\"sha256\":\"" + shaHex + "\"");
    FaceRuntimeProfile p;
    std::string err;
    ASSERT_TRUE(FaceRuntimeManifest::parseV1(manifest, "arm64-v8a", p, err));
    EXPECT_FALSE(FaceRuntimeManifest::verifyFiles(p, tempBase().string(), err));
    EXPECT_NE(err.find("SHA-256"), std::string::npos);
}