#include "EventManager.h"
#include "Storage.h"
#include <gtest/gtest.h>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace rk_core;

// Stubs for Storage methods used by EventManager to avoid linking full Storage class
// which requires opencv
namespace rk_core {
bool Storage::ensureDirectory(const std::string& path) { return true; }
bool Storage::appendLog(const std::string& filename, const std::string& content) { return true; }
void Storage::cleanupOldData(const std::string& directoryPath, int daysToKeep) {}
bool Storage::saveImage(const std::string& filename, const cv::Mat& frame) { return true; }
bool Storage::hasEnoughSpace(const std::string& path, size_t minBytes) { return true; }

TEST(EventManager, FormatJson) {
    EventManager manager;
    AppEvent evt;
    evt.eventId = "abc12345";
    evt.type = "AUTH_FAIL";
    evt.description = "Failed login attempt";
    evt.timestamp = 1672531200;
    evt.snapshotPath = "/tmp/snap.jpg";

    std::string json = manager.formatEventJson(evt);

    EXPECT_NE(json.find("\"id\": \"abc12345\""), std::string::npos);
    EXPECT_NE(json.find("\"type\": \"AUTH_FAIL\""), std::string::npos);
    EXPECT_NE(json.find("\"desc\": \"Failed login attempt\""), std::string::npos);
    EXPECT_NE(json.find("\"ts\": 1672531200"), std::string::npos);
    EXPECT_NE(json.find("\"img\": \"/tmp/snap.jpg\""), std::string::npos);
}

TEST(EventManager, UniqueId) {
    EventManager manager;
    std::set<std::string> ids;

    for (int i = 0; i < 100; ++i) {
        std::string id = manager.generateUniqueId();

        EXPECT_EQ(id.length(), 8);
        if (id.length() != 8) {
            // Early continue to avoid out-of-bounds checks on short strings
            continue;
        }

        // Check charset (hexadecimal)
        for (char c : id) {
            EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }

        // Check uniqueness
        EXPECT_EQ(ids.find(id), ids.end());
        ids.insert(id);
    }
}

// ── FACE-CLEAN-021 回归守卫：生产头文件不得依赖 googletest ──
// 为什么需要它：`src/cpp/include/**` 下的头文件会被**生产 target**（rk3288_cli / native-lib /
// win_* 等）包含，而这些 target 没有 googletest 包含路径。历史上 `EventManager.h` 为支持一条
// FRIEND_TEST 而 `#include <gtest/gtest_prod.h>`，直接导致 `rk3288_cli` 编译失败（C1083），
// 且因 CI 的 windows job 不构建该 target 而**长期静默**。本用例把这条边界变成可执行断言。
//
// 这是**结构式测试**（读源码文本），同仓既有做法——见 `test_checked_arith.cpp` 对
// `RK_TEST_SOURCE_ROOT` 的用法。测试会**剥离 `//` 与 `/* */` 注释（含跨行）**后再识别 `#include`
// 指令，故本文件与 `Engine.h` 里讨论 gtest 的说明性注释不会造成误报。
//
// 若将来确实需要生产头文件里的测试接缝，**正解是上面 EventManager 演示的前向声明 + friend**，
// 而不是把 googletest 拉进生产编译单元。
#ifdef RK_TEST_SOURCE_ROOT
namespace {

// 判断一个头文件里是否有**生效的** gtest/gmock include，有则记入 offenders。
// 实现要点（逐条对应 A2 审计 L1 指出的盲区）：
//   · 先**逐行剥离** `//` 行注释与 `/* */` 块注释（块注释状态**跨行**保留）——只用
//     「行首是不是 `*`/`//`」的启发式会漏掉块注释里不以 `*` 开头的续行（会误报）；
//   · 剥离后再**去掉全部空白**做匹配，故 `#  include <gtest/...>` 这类写法也能命中；
//   · 同时匹配 `gtest` 与 `gmock`——gmock 头在生产 target 上同样没有包含路径。
void collectGtestIncludes(const std::filesystem::path& file, std::vector<std::string>& offenders) {
    std::ifstream in(file);
    if (!in) return;

    bool inBlockComment = false;
    std::string line;
    std::size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;

        std::string code;
        code.reserve(line.size());
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (inBlockComment) {
                if (line[i] == '*' && i + 1 < line.size() && line[i + 1] == '/') {
                    inBlockComment = false;
                    ++i;
                }
                continue;
            }
            if (line[i] == '/' && i + 1 < line.size()) {
                if (line[i + 1] == '/') break;  // 行注释：本行到此为止
                if (line[i + 1] == '*') {
                    inBlockComment = true;
                    ++i;
                    continue;
                }
            }
            code.push_back(line[i]);
        }

        std::string norm;
        norm.reserve(code.size());
        for (char c : code) {
            if (!std::isspace(static_cast<unsigned char>(c))) norm.push_back(c);
        }
        if (norm.rfind("#include", 0) != 0) continue;
        if (norm.find("gtest") == std::string::npos && norm.find("gmock") == std::string::npos) {
            continue;
        }
        offenders.push_back(file.string() + ":" + std::to_string(lineNo) + "  " + line);
    }
}

}  // namespace

TEST(EventManager, ProductionHeadersDoNotDependOnGtestOrGmock) {
    namespace fs = std::filesystem;

    // 扫描范围（含 A1/A2 双代理指出的盲区修正）：这三处头文件目录都会被**生产 target** 包含
    // （`rk3288_cli` / `native-lib` / `win_*`），而它们都没有 gtest/gmock 的包含路径。
    const fs::path roots[] = {
        fs::path(RK_TEST_SOURCE_ROOT) / "src" / "cpp" / "include",
        fs::path(RK_TEST_SOURCE_ROOT) / "src" / "win" / "include",
        fs::path(RK_TEST_SOURCE_ROOT) / "src" / "cpp" / "jni",
    };
    const char* const kExts[] = {".h", ".hpp", ".inl", ".ipp"};

    std::error_code ec;
    std::vector<std::string> offenders;
    for (const fs::path& root : roots) {
        // ⚠️ 用 `EXPECT_TRUE` + `continue` 而**不是** `ASSERT_TRUE`：后者会**提前中止整个用例**，
        //    连已收集到的 offenders 一起吞掉，让报告看起来像「找不到目录」而不是「有违规」
        //    （子代理 4 S6）。宁可多报一条缺目录，也不能少报违规。
        EXPECT_TRUE(fs::exists(root, ec)) << "找不到头文件根目录: " << root.string();
        if (!fs::exists(root, ec)) continue;

        std::size_t scanned = 0;
        for (auto it = fs::recursive_directory_iterator(root, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = it->path().extension().string();
            bool extOk = false;
            for (const char* e : kExts) {
                if (ext == e) {
                    extOk = true;
                    break;
                }
            }
            if (!extOk) continue;
            ++scanned;
            collectGtestIncludes(it->path(), offenders);
        }
        // 迭代器构造失败会把 `ec` 置位 ⇒ 一次都不进循环 ⇒ 该根**静默扫零文件**、用例仍可能全绿。
        // 故显式要求每个根**至少扫到 1 个头文件**（子代理 4 S6）。
        EXPECT_GT(scanned, 0u) << "该根下未扫到任何头文件（路径写错或迭代失败？）: " << root.string();
    }

    EXPECT_TRUE(offenders.empty())
        << "生产头文件不得 #include googletest/gmock：生产 target 没有该包含路径，会编译失败；"
        << "需要测试接缝时请改用前向声明 + friend（见 EventManager.h 顶部说明）";
    for (const auto& o : offenders) {
        ADD_FAILURE() << o;
    }
}
#else
// 这条 `#error` 是**故意**的（子代理 4 S6）：若本文件被编进一个未定义 `RK_TEST_SOURCE_ROOT` 的 target，
// 整段守卫——含 `TEST` 本身——会**静默消失**（既不注册、也不 skip、也不 fail，零信号）。
// 宁可让构建当场失败，也不要留一个看起来在守、实际不存在的守卫。
#error "本文件的结构式守卫依赖 RK_TEST_SOURCE_ROOT；缺失它会让守卫静默消失，请为该 target 定义该宏"
#endif  // RK_TEST_SOURCE_ROOT
} // namespace rk_core
