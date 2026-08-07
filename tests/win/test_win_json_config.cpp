#include "rk_win/WinJsonConfig.h"
#include "rk_win/JsonLite.h"
#include "rk_win/JsonSchemaValidator.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

// RK_WCFR_CONFIG 环境变量注入临时 config.json 路径（defaultConfigPath 读取）。
// 析构时删除环境变量，避免污染同进程其他测试的 WinJsonConfigStore 构造。
struct ConfigEnvGuard {
    explicit ConfigEnvGuard(const std::filesystem::path& cfgPath) {
        SetEnvironmentVariableW(L"RK_WCFR_CONFIG", cfgPath.wstring().c_str());
    }
    ~ConfigEnvGuard() { SetEnvironmentVariableW(L"RK_WCFR_CONFIG", NULL); }
};

std::filesystem::path makeTempDir(const std::string& suffix) {
    const auto base = std::filesystem::temp_directory_path() / "rk3288_opencv_win_json_config_tests";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    const auto dir = base / suffix;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// 外部写入：追加空白到 config.json 触发 mtime 变化（JSON 尾随空白合法，语义不变）
bool touchConfigFile(const std::filesystem::path& cfgPath) {
    std::ofstream f(cfgPath, std::ios::app | std::ios::binary);
    if (!f.is_open()) return false;
    f << " \n";
    return static_cast<bool>(f);
}

}  // namespace

// PUT（updateFromJsonBody）成功后配置代数 +1，消费方可感知并恰好消费一次。
// 修复前：PUT 落盘自更新 lastWriteTime_，pollReloadOnce 检测不到，事件丢失。
bool test_config_put_bumps_generation() {
    const auto dir = makeTempDir("put_gen");
    ConfigEnvGuard env(dir / "config.json");

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    const std::uint64_t gen0 = store.generation();
    const auto res = store.updateFromJsonBody("{\"camera\":{\"width\":800}}");
    if (!res.ok) return false;
    const std::uint64_t gen1 = store.generation();
    return gen1 == gen0 + 1;
}

// 外部写入 config.json：pollReloadOnce 感知并 +1；再次轮询无新变更不重复 +1。
bool test_config_external_write_bumps_generation_once() {
    const auto dir = makeTempDir("ext_gen");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    const std::uint64_t gen0 = store.generation();
    if (!touchConfigFile(cfgPath)) return false;

    bool applied = false;
    std::string err;
    store.pollReloadOnce(applied, err);
    if (!applied) return false;  // 外部变更应被 reload
    const std::uint64_t gen1 = store.generation();
    if (gen1 != gen0 + 1) return false;  // 外部写入 +1

    // 再次轮询：无新变更，不重复 +1
    bool applied2 = false;
    store.pollReloadOnce(applied2, err);
    if (applied2) return false;
    return store.generation() == gen1;
}

// PUT 与外部写入两来源各消费一次、无重复/遗漏（模拟服务主循环 lastGen 去重判定）。
bool test_config_put_and_external_each_consumed_once() {
    const auto dir = makeTempDir("both_gen");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    std::uint64_t lastApplied = store.generation();  // 服务主循环记录的已消费代数
    int consumed = 0;

    // PUT 变更 → 恰好消费一次
    const auto res = store.updateFromJsonBody("{\"camera\":{\"width\":800}}");
    if (!res.ok) return false;
    if (store.generation() != lastApplied) { lastApplied = store.generation(); consumed++; }
    if (consumed != 1) return false;
    // 无新变更 → 不重复消费
    if (store.generation() != lastApplied) consumed++;
    if (consumed != 1) return false;

    // 外部写入变更 → 再消费一次
    if (!touchConfigFile(cfgPath)) return false;
    bool applied = false;
    std::string err;
    store.pollReloadOnce(applied, err);
    if (!applied) return false;
    if (store.generation() != lastApplied) { lastApplied = store.generation(); consumed++; }
    if (consumed != 2) return false;

    return true;
}

// 默认序列化（initialize 生成的配置 JSON）应通过自身 schema。
// 修复前：buildSettingsJson 输出的任何字段若 schema 不允许（如 model.int8Enabled 曾被遗漏）
//   → 校验失败；B06-015 已修 int8Enabled，本测试系统化验证无其他不一致。
bool test_config_default_serialization_passes_schema() {
    const auto dir = makeTempDir("default_schema");
    ConfigEnvGuard env(dir / "config.json");

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    const std::string json = store.currentRedactedJsonPretty();
    rk_win::JsonValue doc;
    std::string perr;
    if (!rk_win::parseJson(json, doc, perr)) return false;
    std::string schemaErr;
    return rk_win::JsonSchemaValidator::validate(
        rk_win::JsonSchemaValidator::buildSettingsDocSchema(), doc, &schemaErr);
}

// 每个单字段 PUT（各对象一个代表性字段）应通过自身 schema（updateFromJsonBody 内校验）。
// 若生成配置（baseDoc）含 schema 不允许的字段，任一 PUT 都会失败。
bool test_config_each_single_field_put_passes_schema() {
    const auto dir = makeTempDir("single_field_schema");
    ConfigEnvGuard env(dir / "config.json");

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    const char* patches[] = {
        "{\"camera\":{\"width\":800}}",
        "{\"recognition\":{\"minFaceSizePx\":80}}",
        "{\"inference\":{\"intervalMs\":300}}",
        "{\"dnn\":{\"inputWidth\":320}}",
        "{\"model\":{\"recognition\":\"arcface\"}}",
        "{\"http\":{\"port\":8081}}",
        "{\"poster\":{\"enable\":true}}",
        "{\"log\":{\"maxRollFiles\":3}}",
        "{\"ui\":{\"windowWidth\":1024}}",
        "{\"display\":{\"width\":1920}}",
        "{\"acceleration\":{\"enableOpenCL\":false}}",
    };
    for (const char* p : patches) {
        const auto res = store.updateFromJsonBody(p);
        if (!res.ok) return false;  // 单字段 PUT 未通过 schema（生成配置含 schema 不允许字段）
    }
    return true;
}
