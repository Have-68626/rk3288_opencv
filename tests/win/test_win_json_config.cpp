#include "rk_win/WinJsonConfig.h"
#include "rk_win/JsonLite.h"
#include "rk_win/JsonSchemaValidator.h"

#include <windows.h>

#include <chrono>
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

// 外部写入：追加空白到 config.json 触发 mtime 变化（JSON 尾随空白合法，语义不变）。
// 追加写后显式设置 mtime +2s：reloadFromDisk 做 lastWriteTime_ == wt 精确比较，
// 粗粒度文件系统（FAT 2s 粒度）或两次写入落同一 tick 时追加写不会刷新 mtime，
// 导致 applied=false 的 flaky（B06-015 遗留）。必须先 close 落盘再设 mtime：
// 若流仍打开，析构 flush 会把 mtime 覆盖回写入时刻，+2s 被抹掉（首版修复教训）；
// 且以写入后的实际 mtime 为基准（而非 now）加 2s，保证与 store 初始化记录必然不同。
bool touchConfigFile(const std::filesystem::path& cfgPath) {
    std::ofstream f(cfgPath, std::ios::app | std::ios::binary);
    if (!f.is_open()) return false;
    f << " \n";
    if (!f) { f.close(); return false; }
    f.close();
    // error_code 重载：与生产 WinJsonConfig.cpp 一致，失败返回 false 而非抛异常中止测试
    std::error_code ec;
    const auto base = std::filesystem::last_write_time(cfgPath, ec);
    if (ec) return false;
    std::filesystem::last_write_time(cfgPath, base + std::chrono::seconds(2), ec);
    return !ec;
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

// B08-004：默认配置“序列化 → 反序列化 → 再序列化”往返不变量 + 与运行时 schema 逐字段对拍。
//
// 判别逻辑（修复前若 C++ 侧存在字段缺口/默认值不一致即失败）：
//   1) 往返不变量：首版脱敏 JSON（currentRedactedJsonPretty，等价于 buildSettingsJson 脱敏输出）
//      经公开 API 路径 updateFromJsonBody 全量写回（内部 = parseAndValidateSettingsDoc + schema 校验 +
//      落盘），再序列化必须与首版逐字符一致。敏感字段走脱敏形态（postUrl="***"），避开加密
//      nonce 随机导致的天然不等；RK_WCFR_* 环境覆盖项会改写解析结果，先清空保证纯默认配置。
//   2) 逐字段对拍：序列化文档顶层段集 == 运行时 buildSettingsDocSchema().properties 段集，
//      且每段字段集双向一致（schema 声明的段必须被序列化，序列化字段不得越出 schema）。
bool test_config_roundtrip_and_schema_alignment() {
    // 清空会改写配置解析结果的环境覆盖项（dnn model/config、http port、poster url）。
    SetEnvironmentVariableW(L"RK_WCFR_DNN_MODEL", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_DNN_CONFIG", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_HTTP_PORT", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_POST_URL", nullptr);

    const auto dir = makeTempDir("roundtrip_alignment");
    ConfigEnvGuard env(dir / "config.json");

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    const std::string json1 = store.currentRedactedJsonPretty();
    if (json1.find("\"_error\"") != std::string::npos) return false;

    // 1) 往返不变量：全量写回再序列化必须与首版一致
    const auto res = store.updateFromJsonBody(json1);
    if (!res.ok) return false;
    const std::string json2 = store.currentRedactedJsonPretty();
    if (json1 != json2) return false;

    // 2) 逐字段对拍：序列化输出字段树与运行时 schema 的段集/字段集双向一致
    rk_win::JsonValue doc;
    std::string perr;
    if (!rk_win::parseJson(json2, doc, perr) || !doc.isObject()) return false;
    const rk_win::JsonValue schema = rk_win::JsonSchemaValidator::buildSettingsDocSchema();
    const rk_win::JsonValue* schemaProps = schema.find("properties");
    if (!schemaProps || !schemaProps->isObject()) return false;

    // 段集一致：doc 顶层键集合 == schema.properties 键集合
    if (doc.o.size() != schemaProps->o.size()) return false;
    for (const auto& [k, v] : schemaProps->o) {
        if (doc.o.find(k) == doc.o.end()) return false;
        (void)v;
    }

    // 每段字段集一致：schema 声明为 object 且带 properties 的段，doc 段字段集必须相同
    for (const auto& [k, v] : schemaProps->o) {
        if (!v.isObject()) continue;
        const rk_win::JsonValue* sp = v.find("properties");
        if (!sp || !sp->isObject()) continue;  // schemaVersion 等标量段跳过
        const auto it = doc.o.find(k);
        if (it == doc.o.end() || !it->second.isObject()) return false;  // schema 段必须被序列化
        if (it->second.o.size() != sp->o.size()) return false;
        for (const auto& [fk, fv] : sp->o) {
            if (it->second.o.find(fk) == it->second.o.end()) return false;
            (void)fv;
        }
    }
    return true;
}

// B08-004-R2：脱敏全量回写不得销毁敏感字段真实值（"***" 哨兵语义 = 不修改）。
//
// 场景：API 调用者 GET settings（currentRedactedJsonPretty）拿到敏感字段的 "***" 占位，
// 直接把该输出全量回写 updateFromJsonBody。修复前 deepMergeObject 把 base 中的
// encrypted-object 覆盖为字面量 "***"；校验仅拦 isObject()，字符串 "***" 直接收下，
// parseAndValidateSettingsDoc 置 postUrl="***"，buildSettingsJson 重加密落盘 → 真实密文永久丢失。
//
// 判别：先设置真实 postUrl，再回写 "***" 占位；updateFromJsonBody 必须成功（正常合并语义
// 不变），且 store.current().poster.postUrl 必须仍是真实值（修复前为字面量 "***"）——用
// current() 访问器读内存明文，避免脱敏输出恒为 "***" 掩盖破坏（脱敏相等不可判）。
bool test_config_masked_echo_preserves_real_value() {
    // 清空环境覆盖项，避免改写解析结果（同 roundtrip 测试）
    SetEnvironmentVariableW(L"RK_WCFR_DNN_MODEL", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_DNN_CONFIG", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_HTTP_PORT", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_POST_URL", nullptr);

    const auto dir = makeTempDir("masked_echo");
    ConfigEnvGuard env(dir / "config.json");

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    const std::string realUrl = "https://real.example/api/faces";
    const auto resSet = store.updateFromJsonBody(
        "{\"poster\":{\"postUrl\":\"" + realUrl + "\"}}");
    if (!resSet.ok) return false;
    if (store.current().poster.postUrl != realUrl) return false;

    // 模拟脱敏回写：echo GET 输出的 "***" 占位，同时携带一个正常字段变更（enable）
    const auto resEcho = store.updateFromJsonBody(
        "{\"poster\":{\"postUrl\":\"***\",\"enable\":true}}");
    if (!resEcho.ok) return false;

    // 修复前：postUrl 被覆盖为字面量 "***"（RED 失败点）
    if (store.current().poster.postUrl != realUrl) return false;
    // 正常字段应照常合并：哨兵剔除不得误伤同对象其他字段
    return store.current().poster.enable;
}

// FACE-ARCWIN-006/007 迁移不变式：既有 config.json 缺 identifyThresholdArcFace 时必须被接受并取默认 0.45。
//
// 为什么必须有这条：identifyThresholdArcFace 是本批新增键，**刻意不进 schema.required**
// （既有部署的 config.json 都没有该键；若设为必填，这些配置会在启动校验时报「缺少必填字段」而被拒
// ⇒ 升级即损坏配置）。现有 roundtrip 用例喂入的是**默认序列化**（已含该键），因此**没有**任何测试
// 覆盖「缺该键的既有配置」这条迁移路径。本用例手工构造一份缺该键的配置走 store.initialize（启动加载）
// 路径，钉住「加载成功 + 缺键落到默认 0.45」。若未来有人把该键误加进 required，或解析处不再是
// 「键缺失时保留默认」，本用例立即变红。
bool test_config_missing_arcface_threshold_key_migrates() {
    // 清空会改写配置解析结果的环境覆盖项（与 roundtrip 测试一致，保证加载结果确定性）
    SetEnvironmentVariableW(L"RK_WCFR_DNN_MODEL", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_DNN_CONFIG", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_HTTP_PORT", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_POST_URL", nullptr);

    const auto dir = makeTempDir("missing_arcface_threshold");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 1) 先让 store 落盘一份默认配置（含新键），作为「既有配置」的基底。
    {
        rk_win::WinJsonConfigStore store;
        std::string warn;
        store.initialize(warn);
    }

    // 2) 从落盘文件剔除 identifyThresholdArcFace，模拟「升级前写入、无该键」的既有配置。
    std::string txt;
    {
        std::ifstream in(cfgPath, std::ios::binary);
        if (!in.is_open()) return false;
        txt.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    rk_win::JsonValue doc;
    std::string perr;
    if (!rk_win::parseJson(txt, doc, perr) || !doc.isObject()) return false;
    rk_win::JsonValue* rec = doc.find("recognition");
    if (!rec || !rec->isObject()) return false;
    // 守卫：默认序列化必须含该键，否则本用例根本没在测「缺键」（防静默通过，同 roundtrip 的防呆思路）。
    if (rec->o.erase("identifyThresholdArcFace") != 1) return false;
    {
        std::ofstream out(cfgPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << rk_win::toJsonString(doc, true);
        if (!out) return false;
    }

    // 3) 启动加载该缺键配置：必须被接受（warn 为空）且取默认 0.45。
    rk_win::WinJsonConfigStore store2;
    std::string warn;
    store2.initialize(warn);
    if (!warn.empty()) return false;  // 缺键配置被拒（如误加入 required）即为迁移破坏
    return store2.current().recognition.identifyThresholdArcFace == 0.45;
}

// FACE-ARCWIN-007 边界：identifyThresholdArcFace 值域 [0,1]（余弦相似度），越界值必须被 schema 拒绝。
//
// 成本低但价值高：55.0 是 LBPH 的卡方距离量纲值——若被误收下，ArcFace 侧 `bestSim >= 55.0`
// （bestSim ∈ [-1,1]）将恒假，identify() 恒返回 UNKNOWN（正是 FACE-ARCWIN-006 的形态）。
// updateFromJsonBody 内部走 buildSettingsDocSchema 校验，55.0 超上界 1.0 ⇒ res.ok 必须为 false。
bool test_config_arcface_threshold_out_of_range_rejected() {
    const auto dir = makeTempDir("arcface_threshold_range");
    ConfigEnvGuard env(dir / "config.json");

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    const auto res = store.updateFromJsonBody("{\"recognition\":{\"identifyThresholdArcFace\":55}}");
    return !res.ok;
}
