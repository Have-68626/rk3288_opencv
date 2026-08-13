// S-SEC-B10-001 恶意 param 负面测试（CVE-2026-50144 前置防护）
//
// ncnn 20260113（本仓库 Android 构建所依赖版本）受 CVE-2026-50144 影响：
// ParamDict::load_param 仅校验 key id 上限（id >= NCNN_MAX_PARAM_COUNT，=32），
// 未校验下限；负 id（[-23299,-1]）绕过 is_array 归一化后直接作为数组下标，
// 产生可控偏移的越界堆写（写前越界 / 相邻堆对象静默破坏）。
//
// 上游尚无含修复的官方发布（20260526 即公告确认的脆弱提交 e54f7b1），
// 因此在本仓库模型加载路径前置文本校验，镜像上游修复语义：
//   归一化后 id < 0 或 id >= 32 均拒绝；数组长度超上限（huge-alloc DoS）拒绝。
//
// 本测试为纯文本校验（不依赖 ncnn/OpenCV），随 core_unit_tests 在 CI
// （RK_SKIP_OPENCV=ON）下构建运行。RED→GREEN：先注册本文件（守卫函数
// 不存在 → 构建失败 = RED），再实现 NcnnParamGuard.h（→ GREEN）。
#include "NcnnParamGuard.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

// 跨进程唯一临时文件后缀（同 test_file_hash.cpp 约定）
std::string uniqueFileSuffix() {
    return "_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

// RAII 清理临时文件
class TempFileCleaner {
public:
    explicit TempFileCleaner(const std::filesystem::path& path) : path_(path) {}
    ~TempFileCleaner() {
        if (std::filesystem::exists(path_)) {
            std::filesystem::remove(path_);
        }
    }
private:
    std::filesystem::path path_;
};

// 写入临时 param 文件并返回路径
std::filesystem::path writeTempParam(const std::string& content) {
    std::filesystem::path tempPath = std::filesystem::temp_directory_path() /
        ("ncnn_param_guard" + uniqueFileSuffix() + ".param");
    std::ofstream out(tempPath, std::ios::binary);
    if (!out) {
        ADD_FAILURE() << "failed to create temp param file: " << tempPath;
        return {};
    }
    out << content;
    out.close();
    return tempPath;
}

// CVE-2026-50144 官方 PoC：负 key id=-1 触发写前越界（id=-1，value=0x12345678）
const char* kCvePoC =
    "7767517\n"
    "1 1\n"
    "Input data 0 1 data -1=305419896\n";

// 良性 param：全部 key id ∈ [0,32)，无数组 key（模拟真实 arcface.param 结构）
const char* kBenignScalar =
    "7767517\n"
    "2 2\n"
    "Input                    in0                      0 1 in0\n"
    "Convolution              conv_0                   1 1 in0 out 0=64 1=3 11=3 12=1 13=1 14=1 2=1 3=1 4=1 5=1 6=1728\n";

// 良性 param：含合法数组 key（-23300 归一化为 id 0，长度 5 随后以逗号分隔值）
const char* kBenignArray =
    "7767517\n"
    "1 1\n"
    "Convolution conv 1 1 data out 0=16 1=3 -23300=5,0.1,0.2,0.4,0.8,1.0\n";

// 危险范围边界：-23299（最接近数组阈值的负标量 id）仍须拒绝
const char* kNegIdUpperBoundary =
    "7767517\n"
    "1 1\n"
    "Input data 0 1 data -23299=7\n";

// 归一化后超上限的数组 key：-23332 → id=32，须拒绝
const char* kArrayIdOutOfRange =
    "7767517\n"
    "1 1\n"
    "Input data 0 1 data -23332=5,0.1,0.2,0.4,0.8,1.0\n";

// 数组长度超上限（huge-alloc DoS 变体）须拒绝
const char* kArrayLenHuge =
    "7767517\n"
    "1 1\n"
    "Input data 0 1 data -23300=999999999,0.1\n";

} // namespace

// 官方 PoC 必须被拒绝（CVE-2026-50144 复现）
TEST(NcnnParamGuard, RejectsCve202650144Poc) {
    std::string err;
    EXPECT_FALSE(rk_core::validateNcnnParamText(kCvePoC, err)) << "PoC must be rejected";
    EXPECT_FALSE(err.empty());
}

// 危险区间 [-23299,-1] 任一负标量 id 必须被拒绝
TEST(NcnnParamGuard, RejectsNegativeIdDangerousRange) {
    for (int id = -23299; id <= -1; ++id) {
        std::string param =
            "7767517\n1 1\nInput data 0 1 data " + std::to_string(id) + "=1\n";
        std::string err;
        EXPECT_FALSE(rk_core::validateNcnnParamText(param, err))
            << "negative id " << id << " must be rejected";
    }
}

// 边界：-23299 拒绝；-23300（数组归一化为 0）放行
TEST(NcnnParamGuard, ArrayThresholdBoundary) {
    std::string err;
    EXPECT_FALSE(rk_core::validateNcnnParamText(kNegIdUpperBoundary, err));
    EXPECT_TRUE(rk_core::validateNcnnParamText(kBenignArray, err)) << err;
}

// 归一化后 id >= 32 拒绝（镜像 ncnn 上限校验）
TEST(NcnnParamGuard, RejectsIdAtOrAboveMax) {
    for (int id = 32; id <= 40; ++id) {
        std::string param =
            "7767517\n1 1\nInput data 0 1 data " + std::to_string(id) + "=1\n";
        std::string err;
        EXPECT_FALSE(rk_core::validateNcnnParamText(param, err))
            << "id " << id << " must be rejected";
    }
    std::string err;
    EXPECT_FALSE(rk_core::validateNcnnParamText(kArrayIdOutOfRange, err));
}

// 数组长度超上限拒绝（huge-alloc DoS 变体）
TEST(NcnnParamGuard, RejectsOversizedArrayLength) {
    std::string err;
    EXPECT_FALSE(rk_core::validateNcnnParamText(kArrayLenHuge, err));
}

// 良性 param（纯标量 + 含合法数组）必须放行，不得误拒真实模型
TEST(NcnnParamGuard, AcceptsBenignParams) {
    std::string err;
    EXPECT_TRUE(rk_core::validateNcnnParamText(kBenignScalar, err)) << err;
    EXPECT_TRUE(rk_core::validateNcnnParamText(kBenignArray, err)) << err;
    EXPECT_TRUE(err.empty());
}

// 文件入口与文本入口行为一致
TEST(NcnnParamGuard, FileEntryMatchesTextEntry) {
    std::string err;

    std::filesystem::path bad = writeTempParam(kCvePoC);
    if (bad.empty()) return;
    TempFileCleaner badCleaner(bad);
    EXPECT_FALSE(rk_core::validateNcnnParamFile(bad.string().c_str(), err));

    std::filesystem::path good = writeTempParam(kBenignScalar);
    if (good.empty()) return;
    TempFileCleaner goodCleaner(good);
    err.clear();
    EXPECT_TRUE(rk_core::validateNcnnParamFile(good.string().c_str(), err)) << err;

    // 不存在的文件须报错（防御：缺失 param 直接拒绝，避免走到 ncnn 空路径）
    std::filesystem::path missing = std::filesystem::temp_directory_path() /
        ("ncnn_param_guard_missing" + uniqueFileSuffix() + ".param");
    err.clear();
    EXPECT_FALSE(rk_core::validateNcnnParamFile(missing.string().c_str(), err));
}

// 真实仓库模型 param 无误拒（fileExists skip 模式：模型文件 gitignored，CI 无此目录则跳过）
// 本地开发/真机目录存在 models/ 与 deps/insightface/ 时，逐文件校验守卫全部放行。
TEST(NcnnParamGuard, AcceptsRealRepoParamsIfPresent) {
    std::error_code ec;
    std::vector<std::filesystem::path> found;
    for (const char* dir : {"models", "deps/insightface"}) {
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& ent : std::filesystem::recursive_directory_iterator(dir, ec)) {
            if (!ent.is_regular_file(ec) || ent.path().extension() != ".param") continue;
            // .pnnx.param 为 PNNX 原始中间产物（非 ncnn 直接加载格式，含 #id= blob 标注），
            // 不在运行时 load_param 路径上，跳过
            if (ent.path().filename().string().find(".pnnx.") != std::string::npos) continue;
            found.push_back(ent.path());
        }
    }
    if (found.empty()) {
        GTEST_SKIP() << "仓库内无 .param 模型文件（gitignored），跳过真实模型校验";
    }
    std::string err;
    for (const auto& p : found) {
        err.clear();
        EXPECT_TRUE(rk_core::validateNcnnParamFile(p.string().c_str(), err))
            << "真实模型 param 不应被守卫误拒: " << p << " (" << err << ")";
    }
}
