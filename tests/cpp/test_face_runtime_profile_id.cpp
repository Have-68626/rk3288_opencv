#include "FaceRuntimeProfile.h"

#include <gtest/gtest.h>

#include <string>

// ══════════════════════════════════════════════════════════════════════════════
// profileId 字符集校验的契约测试（Java FaceModelManifest 的 C++ 镜像）
//
// FaceModelManifest.java 的类注释要求「字段名、缺省值、**非法判定**必须与 C++ 一致」。
// Java 侧新增的 profileId 校验规则（拒绝当且仅当满足任一条）：
//   ① 任一字符 c 满足 c < 0x20（C0 控制字符，含 \n \r \t）或 c == 0x7F；
//   ② 首字符或末字符是空格 0x20（首尾空白）。
// 放行：'='、内部空格、'.'、非 ASCII 等；不加长度上限。
//
// 本文件把该规则在 **C++ 权威侧**（FaceRuntimeManifest::parseV1）钉死。C++ 无状态枚举——
// parseV1 以 bool + err 表达，对应 Java 侧经 FaceModelManager 归入 Status.MANIFEST_INVALID。
//
// 放在 core_unit_tests（RK_SKIP_OPENCV）：parseV1 不依赖 OpenCV/ncnn。
// ══════════════════════════════════════════════════════════════════════════════

namespace {

using rk_core::FaceRuntimeManifest;
using rk_core::FaceRuntimeProfile;

// 合法 manifest 骨架；profileId 的 JSON 片段由调用方注入（可含转义序列）。
std::string manifestWithProfileId(const std::string& profileIdJsonToken) {
    return
        "{\"schemaVersion\":1,"
        "\"profileId\":" + profileIdJsonToken + ",\"displayName\":\"RK3288\","
        "\"supportedAbis\":[\"arm64-v8a\"],"
        "\"detector\":{\"param\":{\"path\":\"scrfd/scrfd.param\"},"
        "\"bin\":{\"path\":\"scrfd/scrfd.bin\"}},"
        "\"embedder\":{\"param\":{\"path\":\"arc/arc.param\"},"
        "\"bin\":{\"path\":\"arc/arc.bin\"}},"
        "\"detectionConfidence\":0.55,\"nmsIou\":0.40,\"identitySimilarity\":0.45}";
}

bool parseProfileIdToken(const std::string& profileIdJsonToken, FaceRuntimeProfile& out) {
    std::string err;
    return FaceRuntimeManifest::parseV1(manifestWithProfileId(profileIdJsonToken),
                                        "arm64-v8a", out, err);
}

}  // namespace

// ── 放行：普通标识符 ─────────────────────────────────────────────────────────
TEST(FaceRuntimeProfileId, AcceptsPlainIdentifier) {
    FaceRuntimeProfile p;
    EXPECT_TRUE(parseProfileIdToken("\"rk3288_light\"", p));
    EXPECT_EQ(p.profileId, "rk3288_light");
}

// ── 放行：'='、内部空格、'.'、非 ASCII ───────────────────────────────────────
TEST(FaceRuntimeProfileId, AcceptsEqualsInternalSpaceDotAndNonAscii) {
    FaceRuntimeProfile p;
    // "配置 v1.0=x" —— 非 ASCII（UTF-8）+ 内部空格 + '.' + '='，全部应放行。
    ASSERT_TRUE(parseProfileIdToken("\"\xE9\x85\x8D\xE7\xBD\xAE v1.0=x\"", p));
    EXPECT_EQ(p.profileId, "\xE9\x85\x8D\xE7\xBD\xAE v1.0=x");
}

// ── 放行：无长度上限（Java 侧 longProfileIdDoesNotTruncateCommitEvidence 依赖此性质）──
TEST(FaceRuntimeProfileId, AcceptsLongIdWithoutLengthLimit) {
    const std::string id(300, 'a');  // 远超任何 240 级上限
    FaceRuntimeProfile p;
    ASSERT_TRUE(parseProfileIdToken("\"" + id + "\"", p));
    EXPECT_EQ(p.profileId, id);
}

// ── 拒绝：首尾空格 ───────────────────────────────────────────────────────────
TEST(FaceRuntimeProfileId, RejectsLeadingSpace) {
    FaceRuntimeProfile p;
    EXPECT_FALSE(parseProfileIdToken("\" rk3288\"", p));
}

TEST(FaceRuntimeProfileId, RejectsTrailingSpace) {
    FaceRuntimeProfile p;
    EXPECT_FALSE(parseProfileIdToken("\"rk3288 \"", p));
}

// ── 拒绝：C0 控制字符（\n \t \r）───────────────────────────────────────────
TEST(FaceRuntimeProfileId, RejectsNewlineControlChar) {
    FaceRuntimeProfile p;
    EXPECT_FALSE(parseProfileIdToken("\"rk\\n3288\"", p));  // JSON \n → 实际 0x0A
}

TEST(FaceRuntimeProfileId, RejectsTabControlChar) {
    FaceRuntimeProfile p;
    EXPECT_FALSE(parseProfileIdToken("\"rk\\t3288\"", p));  // 实际 0x09
}

TEST(FaceRuntimeProfileId, RejectsCarriageReturnControlChar) {
    FaceRuntimeProfile p;
    EXPECT_FALSE(parseProfileIdToken("\"rk\\r3288\"", p));  // 实际 0x0D
}

// ── 拒绝：0x7F (DEL) 与 U+001F（<0x20 的非空白 C0 字符）────────────────────────
TEST(FaceRuntimeProfileId, RejectsDelChar) {
    FaceRuntimeProfile p;
    EXPECT_FALSE(parseProfileIdToken("\"rk\\u007f3288\"", p));  // DEL 0x7F
}

TEST(FaceRuntimeProfileId, RejectsUnitSeparatorChar) {
    FaceRuntimeProfile p;
    EXPECT_FALSE(parseProfileIdToken("\"rk\\u001f3288\"", p));  // 0x1F < 0x20
}

// ── 承重：控制字符出现在**中间**；只查首尾会漏（首尾检查不覆盖内部）─────────────
TEST(FaceRuntimeProfileId, RejectsInteriorControlChar) {
    FaceRuntimeProfile p;
    // 首字符 'a'、末字符 'z' 均非空格；内部含 \n —— 必须具备逐字符扫描才拒。
    EXPECT_FALSE(parseProfileIdToken("\"ab\\nz\"", p));
}
