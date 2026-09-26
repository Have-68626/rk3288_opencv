#include "FaceRuntimeProfile.h"
#include "adapters/ScrfdNcnnAdapter.h"
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

// P1-5：带真实 SCRFD 解码语义的 manifest（原生输出节点名 + distance + 2 anchor + nchw）。
std::string manifestWithDetectorRuntime() {
    return
        "{\"schemaVersion\":1,"
        "\"profileId\":\"rk3288_light\",\"displayName\":\"RK3288 Light\","
        "\"supportedAbis\":[\"armeabi-v7a\",\"arm64-v8a\"],"
        "\"detector\":{\"param\":{\"path\":\"scrfd/scrfd.param\"},\"bin\":{\"path\":\"scrfd/scrfd.bin\"},"
        " \"input\":\"in0\","
        " \"outputs\":[\"score_8\",\"score_16\",\"score_32\",\"bbox_8\",\"bbox_16\",\"bbox_32\","
        " \"kps_8\",\"kps_16\",\"kps_32\"],"
        " \"inputWidth\":640,\"inputHeight\":640,"
        " \"decodeMode\":\"distance\",\"anchorsPerPos\":[2,2,2],\"outputLayout\":\"nchw\"},"
        "\"embedder\":{\"param\":{\"path\":\"arc/arc.param\"},\"bin\":{\"path\":\"arc/arc.bin\"},"
        " \"input\":\"in0\",\"output\":\"out0\",\"inputWidth\":112,\"inputHeight\":112,\"embeddingDim\":512},"
        "\"detectionConfidence\":0.55,\"nmsIou\":0.40,\"identitySimilarity\":0.45,"
        "\"modelVersion\":\"1\",\"licenseId\":\"apache-2.0\"}";
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

// P1-5：detector 新字段解析 + ScrfdNcnnConfig/DecodeOptions 装配。
TEST_F(FaceRuntimeManifestTest, DetectorRuntimeFieldsParsedAndAssembled) {
    FaceRuntimeProfile p;
    std::string err;
    ASSERT_TRUE(FaceRuntimeManifest::parseV1(manifestWithDetectorRuntime(), "arm64-v8a", p, err))
        << err;
    EXPECT_EQ(static_cast<int>(p.detectorDecodeMode),
              static_cast<int>(FaceRuntimeProfile::DetectorDecodeMode::kDistance));
    EXPECT_EQ(p.detectorAnchorsPerPos[0], 2);
    EXPECT_EQ(p.detectorAnchorsPerPos[1], 2);
    EXPECT_EQ(p.detectorAnchorsPerPos[2], 2);
    EXPECT_EQ(static_cast<int>(p.detectorOutputLayout),
              static_cast<int>(FaceRuntimeProfile::DetectorOutputLayout::kNChw));

    std::string aerr;
    const ScrfdNcnnConfig cfg =
        ScrfdNcnnAdapter::configFromProfile(p, "scrfd.param", "scrfd.bin", aerr);
    EXPECT_TRUE(aerr.empty()) << aerr;
    EXPECT_EQ(cfg.paramPath, "scrfd.param");
    EXPECT_EQ(cfg.binPath, "scrfd.bin");
    EXPECT_EQ(cfg.inputName, "in0");
    // 节点名按 manifest 顺序 score×3 / bbox×3 / kps×3 映射
    EXPECT_EQ(cfg.scoreNames[0], "score_8");
    EXPECT_EQ(cfg.scoreNames[2], "score_32");
    EXPECT_EQ(cfg.bboxNames[1], "bbox_16");
    EXPECT_EQ(cfg.kpsNames[2], "kps_32");
    EXPECT_EQ(cfg.inputW, 640);
    EXPECT_EQ(cfg.inputH, 640);
    EXPECT_EQ(cfg.stride[0], 8);
    EXPECT_EQ(cfg.stride[1], 16);
    EXPECT_EQ(cfg.stride[2], 32);
    EXPECT_EQ(static_cast<int>(cfg.decodeMode),
              static_cast<int>(pipeline::DecodeMode::kDistance));
    EXPECT_EQ(cfg.anchorsPerPos[0], 2);
    EXPECT_EQ(cfg.anchorsPerPos[1], 2);
    EXPECT_EQ(cfg.anchorsPerPos[2], 2);
    EXPECT_EQ(static_cast<int>(cfg.outputLayout),
              static_cast<int>(ScrfdNcnnConfig::OutputLayout::kNChw));
    EXPECT_FLOAT_EQ(cfg.confThreshold, 0.55f);
    EXPECT_FLOAT_EQ(cfg.post.nmsIouThreshold, 0.40f);

    const pipeline::ScrfdDecodeOptions opt = ScrfdNcnnAdapter::decodeOptionsFromProfile(p);
    EXPECT_EQ(static_cast<int>(opt.mode), static_cast<int>(pipeline::DecodeMode::kDistance));
    EXPECT_EQ(opt.numAnchors[0], 2);
    EXPECT_EQ(opt.numAnchors[1], 2);
    EXPECT_EQ(opt.numAnchors[2], 2);
    EXPECT_EQ(opt.stride[0], 8);
    EXPECT_FLOAT_EQ(opt.confThreshold, 0.55f);
    EXPECT_FLOAT_EQ(opt.post.nmsIouThreshold, 0.40f);
}

// 向后兼容：旧 manifest 无新字段时保持 delta / {1,1,1} / auto 默认。
TEST_F(FaceRuntimeManifestTest, DetectorRuntimeDefaultsBackwardCompatible) {
    FaceRuntimeProfile p;
    std::string err;
    ASSERT_TRUE(FaceRuntimeManifest::parseV1(validManifest(), "arm64-v8a", p, err));
    EXPECT_EQ(static_cast<int>(p.detectorDecodeMode),
              static_cast<int>(FaceRuntimeProfile::DetectorDecodeMode::kDelta));
    EXPECT_EQ(p.detectorAnchorsPerPos[0], 1);
    EXPECT_EQ(p.detectorAnchorsPerPos[1], 1);
    EXPECT_EQ(p.detectorAnchorsPerPos[2], 1);
    EXPECT_EQ(static_cast<int>(p.detectorOutputLayout),
              static_cast<int>(FaceRuntimeProfile::DetectorOutputLayout::kAuto));

    std::string aerr;
    const ScrfdNcnnConfig cfg =
        ScrfdNcnnAdapter::configFromProfile(p, "p.param", "b.bin", aerr);
    EXPECT_TRUE(aerr.empty()) << aerr;
    EXPECT_EQ(static_cast<int>(cfg.decodeMode),
              static_cast<int>(pipeline::DecodeMode::kDelta));
    EXPECT_EQ(cfg.anchorsPerPos[0], 1);
    EXPECT_EQ(static_cast<int>(cfg.outputLayout),
              static_cast<int>(ScrfdNcnnConfig::OutputLayout::kAuto));
    // 无新字段 → 沿用 PNNX 约定节点名
    EXPECT_EQ(cfg.scoreNames[0], "out0");
    EXPECT_EQ(cfg.kpsNames[2], "out8");

    const pipeline::ScrfdDecodeOptions opt = ScrfdNcnnAdapter::decodeOptionsFromProfile(p);
    EXPECT_EQ(static_cast<int>(opt.mode), static_cast<int>(pipeline::DecodeMode::kDelta));
    EXPECT_EQ(opt.numAnchors[0], 1);
}

TEST_F(FaceRuntimeManifestTest, MissingDetectorInputFallsBackToRealScrfdBlobName) {
    // 回归护栏（判别性）：**检测器**输入名的缺键回落值必须是本仓 SCRFD .param 的真实 Input 层名
    // `input.1`（`models/scrfd_face_detect_ncnn/scrfd_*-opt2.param` 第 3 行皆为 `Input input.1 0 1 input.1`）。
    // 旧回落值 `in0` 是 **ArcFace 侧**的名字（`models/arcface_ncnn/arcface.param` 第 3 行），曾被误抄到检测器侧；
    // 用错会让 `ScrfdNcnnAdapter` 的 `ex.input()` 报 `ncnn_set_input_failed`。
    // 把回落值改回 "in0" ⇒ 本用例红；把嵌入器回落值也"顺手"改成 input.1 ⇒ 第二断言红。
    const std::string minimal =
        "{"
        "\"schemaVersion\":1,\"profileId\":\"m\",\"displayName\":\"M\","
        "\"supportedAbis\":[\"arm64-v8a\"],"
        "\"detector\":{\"param\":{\"path\":\"d.param\"},\"bin\":{\"path\":\"d.bin\"}},"
        "\"embedder\":{\"param\":{\"path\":\"e.param\"},\"bin\":{\"path\":\"e.bin\"}},"
        "\"detectionConfidence\":0.5,\"nmsIou\":0.4,\"identitySimilarity\":0.4"
        "}";
    FaceRuntimeProfile p;
    std::string err;
    ASSERT_TRUE(FaceRuntimeManifest::parseV1(minimal, "arm64-v8a", p, err)) << err;
    EXPECT_EQ(p.detectorInput, "input.1");
    EXPECT_EQ(p.embedderInput, "in0");
}

TEST_F(FaceRuntimeManifestTest, DetectorDecodeModeInvalidRejected) {
    std::string bad = manifestWithDetectorRuntime();
    auto pos = bad.find("\"decodeMode\":\"distance\"");
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, std::string("\"decodeMode\":\"distance\"").size(),
                "\"decodeMode\":\"bogus\"");
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("decodeMode"), std::string::npos);
}

TEST_F(FaceRuntimeManifestTest, DetectorAnchorsPerPosWrongLengthRejected) {
    std::string bad = manifestWithDetectorRuntime();
    auto pos = bad.find("\"anchorsPerPos\":[2,2,2]");
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, std::string("\"anchorsPerPos\":[2,2,2]").size(),
                "\"anchorsPerPos\":[2,2]");
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("anchorsPerPos"), std::string::npos);
}

TEST_F(FaceRuntimeManifestTest, DetectorAnchorsPerPosNonPositiveRejected) {
    std::string bad = manifestWithDetectorRuntime();
    auto pos = bad.find("\"anchorsPerPos\":[2,2,2]");
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, std::string("\"anchorsPerPos\":[2,2,2]").size(),
                "\"anchorsPerPos\":[2,0,2]");
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("anchorsPerPos"), std::string::npos);
}

// 上限：元素超 kMaxAnchorsPerPos(=8) → 拒（防下游 4*N/10*N 溢出）。
// 说明：本用例（[2,2,9]）与 DetectorAnchorsPerPosHugeValueRejected（[2,2,1e100]）打的是**同一条**
// 「上限」判据——9 只是超上限的「普通」样本，1e100 则额外区分「上限真正生效」与「靠 double→int
// 未定义行为侥幸被拒」。两条用例的断言均**绑定精确的中文错误消息子串**（含 "anchorsPerPos" 与
// "1..8"/"须正整数"），故改动生产侧消息措辞即会令断言变红——这是有意的（消息即接口）。
TEST_F(FaceRuntimeManifestTest, DetectorAnchorsPerPosOverLimitRejected) {
    std::string bad = manifestWithDetectorRuntime();
    auto pos = bad.find("\"anchorsPerPos\":[2,2,2]");
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, std::string("\"anchorsPerPos\":[2,2,2]").size(),
                "\"anchorsPerPos\":[2,2,9]");  // 9 > 8
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("anchorsPerPos"), std::string::npos);
    // 打红点：命中「上限」分支（消息含 1..8）；上限判据被删则落到整数分支（无 1..8）。
    EXPECT_NE(err.find("1..8"), std::string::npos);
}

// 极大值：1e100 有限且 >0，须在 cast int 前被上限拦下（否则 double→int 未定义行为）。
// 说明：与 DetectorAnchorsPerPosOverLimitRejected（[2,2,9]）打的是**同一条上限判据**——2 的增量
// 意义仅在于区分「上限生效」与「靠 1e100→int 的 UB 侥幸被拒」（实测 MSVC x64 下 cast(1e100)==INT_MIN，
// 整数回等判据亦会拒），并非另一套逻辑。断言同样绑定精确消息子串（"anchorsPerPos" + "1..8"）。
TEST_F(FaceRuntimeManifestTest, DetectorAnchorsPerPosHugeValueRejected) {
    std::string bad = manifestWithDetectorRuntime();
    auto pos = bad.find("\"anchorsPerPos\":[2,2,2]");
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, std::string("\"anchorsPerPos\":[2,2,2]").size(),
                "\"anchorsPerPos\":[2,2,1e100]");
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("anchorsPerPos"), std::string::npos);
    // 打红点：必须在「上限」分支被拒（消息含 1..8），而非依赖 1e100→int 的 UB 回落。
    // 实测 MSVC x64 下 cast(1e100)==INT_MIN，整数回等判据（L117）亦会拒——故此断言
    // 是把「上限真正生效」与「靠 UB 侥幸被拒」二者区分开的唯一判据。
    EXPECT_NE(err.find("1..8"), std::string::npos);
}

// 非整数：1.5 通过「有限/正/上限」检查后，仍须被「整数值」分支拒。
TEST_F(FaceRuntimeManifestTest, DetectorAnchorsPerPosNonIntegerRejected) {
    std::string bad = manifestWithDetectorRuntime();
    auto pos = bad.find("\"anchorsPerPos\":[2,2,2]");
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, std::string("\"anchorsPerPos\":[2,2,2]").size(),
                "\"anchorsPerPos\":[2,2,1.5]");
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("anchorsPerPos"), std::string::npos);
    // 打红点：命中「非整数」分支（消息含「须正整数」且不含 1..8）；整数回等判据被删则接受。
    EXPECT_NE(err.find("须正整数"), std::string::npos);
}

TEST_F(FaceRuntimeManifestTest, DetectorOutputLayoutInvalidRejected) {
    std::string bad = manifestWithDetectorRuntime();
    auto pos = bad.find("\"outputLayout\":\"nchw\"");
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, std::string("\"outputLayout\":\"nchw\"").size(),
                "\"outputLayout\":\"xyz\"");
    FaceRuntimeProfile p;
    std::string err;
    EXPECT_FALSE(FaceRuntimeManifest::parseV1(bad, "arm64-v8a", p, err));
    EXPECT_NE(err.find("outputLayout"), std::string::npos);
}