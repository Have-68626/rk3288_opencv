// S-SEC-B07-001 日志脱敏（native NativeLog 通用脱敏层）
//
// 背景：Java 侧 AppLog 已对 INFO+ 日志经 SensitiveDataUtil 掩码后落盘，但 native 侧
// NativeLog 逐字写盘/logcat，无任何脱敏——native 打出的敏感 KV（password=/token=/
// api_key= 等）会原样落盘。
//
// RED 阶段：本文件先以负面用例证明泄露——敏感 KV 原值出现在写盘日志中。
// GREEN 阶段：实现 rklog::maskSensitiveData 后，同一文件断言翻转为“无原值”，
// 并新增 maskSensitiveData 纯函数单测（可测 seam，不依赖文件/平台）。
//
// 本测试随 core_unit_tests 在 CI（RK_SKIP_OPENCV=ON）下构建运行，不依赖 OpenCV/ncnn。
#include "NativeLog.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// 跨进程唯一临时目录后缀（同 test_file_hash.cpp / test_ncnn_param_guard.cpp 约定）
std::string uniqueSuffix() {
    return "_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

// 读取文件全部内容
std::string readFileAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// GREEN：写盘日志对敏感 KV 统一脱敏，无原值残留。
TEST(NativeLogRedaction, FileSinkRedactsSensitiveKv) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("rk_nativelog_redaction" + uniqueSuffix());
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "rk_redaction.log";

    rklog::setLogDirs(dir.string(), "", "rk_redaction.log");
    rklog::logInfo("SEC", "redactionTest",
                   "login password=superSecret123 api_key=AKIAIOSFODNN7EXAMPLE");
    rklog::setLogDirs("", "", "");  // 关闭 FD 避免占用日志文件

    const std::string content = readFileAll(file);
    std::filesystem::remove_all(dir);

    // 断言写盘内容无敏感原值（RED 阶段此处为“含原值”，GREEN 后翻转）。
    EXPECT_EQ(content.find("superSecret123"), std::string::npos)
        << "password= 原值不得出现在写盘日志";
    EXPECT_EQ(content.find("AKIAIOSFODNN7EXAMPLE"), std::string::npos)
        << "api_key= 原值不得出现在写盘日志";
    // 正向确认：掩码后的值确实写盘（证明是脱敏而非丢弃日志行）。
    EXPECT_NE(content.find("su***23"), std::string::npos)
        << "password= 应写盘为掩码值 su***23";
    EXPECT_NE(content.find("AK***LE"), std::string::npos)
        << "api_key= 应写盘为掩码值 AK***LE";
}

// maskSensitiveData 纯函数单测：敏感 KV 掩码形态（对齐 Java SensitiveDataUtil）。
TEST(NativeLogRedaction, MaskSensitiveKvValues) {
    EXPECT_EQ(rklog::maskSensitiveData("login password=superSecret123 ok"),
              "login password=su***23 ok");              // 首2+***+尾2
    EXPECT_EQ(rklog::maskSensitiveData("token: abc123 done"),
              "token: ab***23 done");                    // 冒号分隔
    EXPECT_EQ(rklog::maskSensitiveData("api_key=AKIAIOSFODNN7EXAMPLE"),
              "api_key=AK***LE");                        // 长值保留首2尾2
    EXPECT_EQ(rklog::maskSensitiveData("secret=pwd"), "secret=***");      // 短值整体***
    EXPECT_EQ(rklog::maskSensitiveData("passwd=12345"), "passwd=12***45");
    EXPECT_EQ(rklog::maskSensitiveData("PASSWORD=Admin123!"), "PASSWORD=Ad***3!");  // 大小写不敏感
    EXPECT_EQ(rklog::maskSensitiveData("bearer=eyJhbGciOiJIUzI1NiJ9"),
              "bearer=ey***J9");
    EXPECT_EQ(rklog::maskSensitiveData("a=1 password=xyz b=2"),
              "a=1 password=*** b=2");                   // 值边界：不吞相邻 KV
    EXPECT_EQ(rklog::maskSensitiveData("redir?token=abcdefgh&page=2"),
              "redir?token=ab***gh&page=2");             // URL query：& 为值边界
    EXPECT_EQ(rklog::maskSensitiveData("access_token=ABCDEFGH12345678"),
              "access_token=AB***78");                   // 复合键
    EXPECT_EQ(rklog::maskSensitiveData("api-key=QWERTY123456"), "api-key=QW***56");  // 连字符变体
}

// RED 补充用例（SEC-04 遗留缺口，P3-4）：
//   缺口 A：Authorization: Bearer <jwt> 原实现值捕获遇空格即停，只掩 "Bearer"，
//           JWT 主体（空格后的整段）原样落盘 → 此处断言输出不含 JWT 原值。
//   缺口 B：secret_key= / client_secret= 因 \bsecret\b 遇下划线（同为词字符）
//           尾边界失效而完全不匹配 → 值明文泄露 → 此处断言输出不含值原值。
TEST(NativeLogRedaction, MaskSensitiveKvBearerTokenAndCompoundKeys) {
    // 缺口 A：JWT 主体必须被掩掉（Bearer 前缀保留，token 整体掩码）。
    EXPECT_EQ(
        rklog::maskSensitiveData(
            "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.signature"),
        "Authorization: Bearer ey***re");
    EXPECT_EQ(rklog::maskSensitiveData(
                  "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.signature")
                  .find("eyJhbGciOiJIUzI1NiJ9.payload.signature"),
              std::string::npos)
        << "Bearer 后的 JWT 主体原值不得出现在输出中";

    // 缺口 B：下划线组合键必须命中并掩码。
    EXPECT_EQ(rklog::maskSensitiveData("secret_key=superSecretVal"),
              "secret_key=su***al");
    EXPECT_EQ(rklog::maskSensitiveData("client_secret=clientVal"),
              "client_secret=cl***al");
    EXPECT_EQ(rklog::maskSensitiveData("secret_key=superSecretVal").find("superSecretVal"),
              std::string::npos)
        << "secret_key= 值原值不得出现在输出中";
    EXPECT_EQ(rklog::maskSensitiveData("client_secret=clientVal").find("clientVal"),
              std::string::npos)
        << "client_secret= 值原值不得出现在输出中";
}

// maskSensitiveData 纯函数单测：不得误伤正常日志。
TEST(NativeLogRedaction, MaskSensitiveKvNoFalsePositive) {
    EXPECT_EQ(rklog::maskSensitiveData(""), "");
    EXPECT_EQ(rklog::maskSensitiveData("frame 42 processed in 12ms"),
              "frame 42 processed in 12ms");             // 普通日志
    EXPECT_EQ(rklog::maskSensitiveData("中文日志 人脸识别成功"),
              "中文日志 人脸识别成功");                   // 中文内容
    EXPECT_EQ(rklog::maskSensitiveData("url=http://example.com:8080/path"),
              "url=http://example.com:8080/path");       // 普通 URL（无敏感键）
    EXPECT_EQ(rklog::maskSensitiveData("keyword=hello"),
              "keyword=hello");                          // 词边界：key 需为独立词
    EXPECT_EQ(rklog::maskSensitiveData("monkey=banana"),
              "monkey=banana");                          // 非敏感键
    EXPECT_EQ(rklog::maskSensitiveData("received 5 tokens"),
              "received 5 tokens");                      // token 作普通词、无 KV 形态
    EXPECT_EQ(rklog::maskSensitiveData("password"),
              "password");                               // 无分隔符不掩
    EXPECT_EQ(rklog::maskSensitiveData("authorization = "),
              "authorization = ");                       // 空值不掩
}
}  // namespace
