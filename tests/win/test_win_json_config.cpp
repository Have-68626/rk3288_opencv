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
#include <vector>

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

// 把文件 mtime 拨快 +2s（以当前实际 mtime 为基准）。用于「已整份覆盖写入新内容」的场景：
// reloadFromDisk 做 lastWriteTime_ == wt 精确比较，粗粒度文件系统（FAT 2s 粒度）或两次写入落同一
// tick 时，覆盖写不会刷新出与 store 记录不同的 mtime，导致 applied=false 的 flaky。须在关闭写流
// 之后调用（touchConfigFile 的同因教训：流仍打开时析构 flush 会把 mtime 覆盖回写入时刻）。
bool bumpMtimePlus2s(const std::filesystem::path& p) {
    std::error_code ec;
    const auto base = std::filesystem::last_write_time(p, ec);
    if (ec) return false;
    std::filesystem::last_write_time(p, base + std::chrono::seconds(2), ec);
    return !ec;
}

// 列出目录下所有 "config.json.corrupt." 前缀文件（FACE-CONFIG-001 隔离命名含毫秒时间戳 ⇒ 不得硬编码文件名）。
std::vector<std::filesystem::path> listCorruptConfigs(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().filename().string().rfind("config.json.corrupt.", 0) == 0) out.push_back(e.path());
    }
    return out;
}

// 读文件全部内容（失败返回 false）。用于逐字节比对隔离产物。
bool readAllText(const std::filesystem::path& p, std::string& out) {
    out.clear();
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// 覆盖写文件全部内容（失败返回 false）。
bool writeAllText(const std::filesystem::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

// 文件的唯一标识（卷序列号 + 文件索引）。重命名（移动）只改目录项、保留 MFT 记录（索引不变）；
// 拷贝会新建文件、得到不同标识。用于判别隔离是「改名（移动）」还是「拷贝」。
struct FileId {
    DWORD volume = 0;
    DWORD indexHigh = 0;
    DWORD indexLow = 0;
};

bool fileIdOf(const std::filesystem::path& p, FileId& idOut) {
    const HANDLE h = CreateFileW(p.wstring().c_str(), 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info) != FALSE;
    CloseHandle(h);
    if (!ok) return false;
    idOut.volume = info.dwVolumeSerialNumber;
    idOut.indexHigh = info.nFileIndexHigh;
    idOut.indexLow = info.nFileIndexLow;
    return true;
}

bool sameFileId(const FileId& a, const FileId& b) {
    return a.volume == b.volume && a.indexHigh == b.indexHigh && a.indexLow == b.indexLow;
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
// 为什么必须有这条：identifyThresholdArcFace 已**收束进 schema.required**（三处同步）。既有部署的
// config.json 都没有该键，若不做补偿，这些配置会在启动校验时报「缺少必填字段」而被拒 ⇒ 降级为默认值
// 运行（历史行为更糟：WinJsonConfigStore::initialize 在校验失败时并不 return，而是穿透到 INI 迁移分支
// 并无条件覆盖写 config.json ⇒ 静默丢弃用户全部配置；FACE-CONFIG-001 已改为先隔离坏文件再以默认值启动）。
// 补偿即 WinJsonConfig.cpp 的 normalizeRecognitionKeys：在
// schema 校验前对 recognition 段「缺键补默认」（补入值取 defaultAppConfig()，与解析侧缺键回退一致）。
// 现有 roundtrip 用例喂入的是**默认序列化**（已含该键），因此**没有**任何测试覆盖「缺该键的既有配置」
// 这条迁移路径。本用例手工构造一份缺该键的配置走 store.initialize（启动加载）路径，钉住
// 「加载成功 + 缺键落到默认 0.45 + 后续落盘重新含该键」的端到端行为。若未来有人删掉规范化、或解析处
// 不再是「键缺失时保留默认」，本用例立即变红。
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

    // 3) 启动加载该缺键配置：规范化在校验前补齐该键 ⇒ 必须被接受（warn 为空）且取默认 0.45。
    rk_win::WinJsonConfigStore store2;
    std::string warn;
    store2.initialize(warn);
    if (!warn.empty()) return false;  // 缺键配置被拒（规范化缺失/误删）即为迁移破坏
    if (store2.current().recognition.identifyThresholdArcFace != 0.45) return false;

    // 4) 端到端落盘：initialize 成功本身不重写 config.json（仅在级联迁移分支落盘），故经一次空 patch
    //    PUT 触发写，断言落盘产物重新含该键且值 = 规范化补入的默认 0.45（防「加载能用但一写就丢」）。
    if (!store2.updateFromJsonBody("{}").ok) return false;
    std::string after;
    {
        std::ifstream in(cfgPath, std::ios::binary);
        if (!in.is_open()) return false;
        after.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    rk_win::JsonValue docAfter;
    std::string aerr;
    if (!rk_win::parseJson(after, docAfter, aerr) || !docAfter.isObject()) return false;
    const rk_win::JsonValue* recAfter = docAfter.find("recognition");
    if (!recAfter || !recAfter->isObject()) return false;
    const rk_win::JsonValue* vAfter = recAfter->find("identifyThresholdArcFace");
    return vAfter && vAfter->isNumber() && vAfter->n == 0.45;
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

// FACE-CONFIG-001 T1：config.json 存在但被 schema 拒绝 ⇒ 先改名另存（保全证据）再以默认值启动。
//
// 判别性断言：隔离文件内容必须与原坏文件**逐字节相同**——否则只是「删掉重建」，不是保全证据。
bool test_config_rejected_load_quarantines_and_degrades() {
    const auto dir = makeTempDir("reject_quarantine");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 1) 先落盘一份默认（合法）配置，作为「真实用户配置」基底。
    {
        rk_win::WinJsonConfigStore s;
        std::string w;
        s.initialize(w);
    }
    // 2) 引入单处类型错误：minFaceSizePx 期望 number，改为字符串 ⇒ JSON 可解析但 schema 拒绝。
    std::string base;
    if (!readAllText(cfgPath, base)) return false;
    rk_win::JsonValue doc;
    std::string perr;
    if (!rk_win::parseJson(base, doc, perr) || !doc.isObject()) return false;
    rk_win::JsonValue* rec = doc.find("recognition");
    if (!rec || !rec->isObject()) return false;
    rec->o["minFaceSizePx"] = rk_win::JsonValue::makeString("80");
    const std::string bad = rk_win::toJsonString(doc, true);
    if (!writeAllText(cfgPath, bad)) return false;

    // 3) 启动装载：坏配置必须被隔离且降级启动。
    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    // (e) 诊断访问器：降级启动 + 有隔离文件名 + 告警副本一致。
    if (!store.configRejectedOnLoad()) return false;
    const std::string qname = store.initQuarantineFileName();
    if (qname.empty()) return false;
    if (store.initWarning() != warn) return false;
    // (d) 告警非空且含隔离文件名。
    if (warn.empty() || warn.find(qname) == std::string::npos) return false;

    // (a) 目录里恰好一个隔离文件，且名字与访问器一致（时间戳 → 只能前缀匹配，不得硬编码）。
    const auto corrupts = listCorruptConfigs(dir);
    if (corrupts.size() != 1) return false;
    if (corrupts[0].filename().string() != qname) return false;
    // (b) 隔离文件内容与原坏文件逐字节相同（保全证据）。
    std::string qtext;
    if (!readAllText(corrupts[0], qtext)) return false;
    if (qtext != bad) return false;

    // (c) config.json 仍存在（以默认值重新落地），且可被再次正常装载。
    if (!std::filesystem::exists(cfgPath)) return false;
    rk_win::WinJsonConfigStore store2;
    std::string warn2;
    store2.initialize(warn2);
    if (!warn2.empty()) return false;
    if (store2.configRejectedOnLoad()) return false;
    return true;
}

// FACE-CONFIG-001 T2：语法坏 JSON（截断）同属「无法装载」⇒ 同样隔离 + 降级，且内容逐字节保全。
// 覆盖 parseAndValidateSettingsDoc 的「解析失败」分支（readFileAll 成功、parseJson 失败）。
bool test_config_parse_failure_quarantines_and_degrades() {
    const auto dir = makeTempDir("parse_quarantine");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    const std::string bad = "{\"camera\": ";  // 截断，parseJson 必然失败
    if (!writeAllText(cfgPath, bad)) return false;

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    if (!store.configRejectedOnLoad()) return false;
    const std::string qname = store.initQuarantineFileName();
    if (qname.empty()) return false;
    if (warn.empty() || warn.find(qname) == std::string::npos) return false;

    const auto corrupts = listCorruptConfigs(dir);
    if (corrupts.size() != 1) return false;
    std::string qtext;
    if (!readAllText(corrupts[0], qtext)) return false;
    if (qtext != bad) return false;
    return true;
}

// FACE-CONFIG-001 T3（负对照）：合法 config.json 不得被隔离，且无告警、非降级启动。
bool test_config_healthy_load_not_quarantined() {
    const auto dir = makeTempDir("healthy_no_quarantine");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 先落盘一份默认（合法）配置。
    {
        rk_win::WinJsonConfigStore s;
        std::string w;
        s.initialize(w);
    }
    if (!std::filesystem::exists(cfgPath)) return false;

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    if (!warn.empty()) return false;
    if (store.configRejectedOnLoad()) return false;
    if (!store.initQuarantineFileName().empty()) return false;
    if (!store.initWarning().empty()) return false;
    if (!listCorruptConfigs(dir).empty()) return false;  // 健康配置绝不产生隔离文件
    return true;
}

// FACE-CONFIG-001 T4（**核心不变量**）：隔离失败时，坏配置**永不**被销毁。
// 修复前的失效模式是「无法装载 ⇒ 无条件覆盖写 config.json」，坏文件被整份丢弃；新语义的底线是：
// 坏文件**要么改名另存、要么原封不动**——绝不允许「既没另存、又被覆盖」这第三种结局。
// 构造手段：以**独占句柄**（dwShareMode = 0）持有 config.json ⇒ 随后对该文件的 rename 会失败
//（Windows 上目标句柄未共享删除权限时 MoveFileEx 报 sharing violation）。此时：
//   · 隔离必然失败 ⇒ initQuarantineFileName() 必须为空、目录里不得出现 *.corrupt.*；
//   · config.json 内容必须与写入时**逐字节相同** —— 这就是「保全证据」的可执行判据。
// 附带：本用例实际走的是「不可读」分支（独占句柄使 ifstream 打开即失败），补齐了此前只覆盖
// 「解析失败」的空白。
// ⚠️ **判别力边界（如实标注）**：本构造下**写盘本身也会失败**——`writeAtomicallyWithBackup` 的
//    `MoveFileExW` 对同一个独占句柄同样报 sharing violation，故「原文件未被销毁」在这里**也会**由
//    「覆盖恰好失败」达成。也就是说，本用例断言的是**结果**，它**不能**把 initialize 里的
//    `skipWrite` 护栏（「隔离失败 ⇒ 不落盘」）与「落盘恰好也失败了」区分开。
//    为补上该护栏的判别力，本用例**另加一条副作用断言**：若 `skipWrite` 被移除而写盘路径被进入，
//    `writeAtomicallyWithBackup` 会在替换目标**之前**先做 `.bak` 轮转（把既有 `config.json.bak`
//    改名为 `.bak.1`）——这一步不触碰被独占句柄锁住的 config.json，因此**必然成功**。
//    于是「`.bak` 原封不动且不存在 `.bak.1`」即可判别「写盘路径未被进入」。
bool test_config_quarantine_failure_preserves_original() {
    const auto dir = makeTempDir("quarantine_failure_preserves");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    const std::string bad = "{\"camera\": { \"fps\": 30 ";  // 截断 JSON；内容独特，便于逐字节比对
    if (!writeAllText(cfgPath, bad)) return false;
    // 预置一份 .bak，用于判别「写盘路径是否被进入」（见上方判别力边界说明）。
    if (!writeAllText(dir / "config.json.bak", "{\"schemaVersion\":1}")) return false;

    const HANDLE hold = CreateFileW(cfgPath.wstring().c_str(), GENERIC_READ, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    // 构造不出场景时必须**响亮失败**，绝不静默跳过——否则本用例会退化成恒真断言。
    if (hold == INVALID_HANDLE_VALUE) return false;

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    const bool rejected = store.configRejectedOnLoad();
    const bool noQuarantineName = store.initQuarantineFileName().empty();
    const bool noCorruptFile = listCorruptConfigs(dir).empty();
    const bool warned = !warn.empty();
    const bool bakUntouched = std::filesystem::exists(dir / "config.json.bak");
    const bool noBakRotation = !std::filesystem::exists(dir / "config.json.bak.1");

    CloseHandle(hold);  // 释放独占句柄，随后才可安全读回原文件

    std::string after;
    const bool readOk = readAllText(cfgPath, after);

    if (!rejected) return false;         // 独占句柄下不可读 ⇒ 必须判定为「无法装载」
    if (!warned) return false;           // 失败必须可见（warnOut，并同时直写 stderr）
    if (!noQuarantineName) return false; // 隔离确实失败了
    if (!noCorruptFile) return false;    // 失败就不该有隔离产物
    if (!bakUntouched) return false;     // .bak 被轮转 ⇒ 写盘路径被进入（skipWrite 护栏失效）
    if (!noBakRotation) return false;    // 同上，双保险（轮转产物出现即证明写盘被调用）
    if (!readOk) return false;
    if (after != bad) return false;      // ★ 核心断言：原文件逐字节未被动过
    return true;
}

// FACE-CONFIG-006（命名格式）+ FACE-CONFIG-007（隔离 = 改名/移动而非拷贝）合并用例。
//
// (1) 命名格式：隔离产物必须为 config.json.corrupt.<十进制毫秒>。断言后缀为纯数字且位数 ≥ 12
//     ——毫秒时间戳自 epoch 约 1.7e12（13 位），秒级约 1.7e9（10 位），故位数可判「毫秒而非秒」；
//     后缀非纯十进制则说明换成了秒/其它后缀（如 .bak）。
// (2) 移动判据：比对隔离**前后**两个文件的唯一标识（卷序列号 + 文件索引）。重命名只改目录项、
//     保留 MFT 记录（文件索引不变）；拷贝会新建文件、得到不同标识。故「标识相同」⟺ 隔离是移动而非拷贝。
//     判别力说明：仅断言「隔离内容与原内容逐字节相同」**不能**区分移动与拷贝（拷贝也满足）；
//     若把 quarantineConfigFile 改成 copy_file + 覆盖原文件，本用例的标识比对立即变红。
bool test_config_quarantine_move_and_name() {
    const auto dir = makeTempDir("quarantine_move_name");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    const std::string bad = "{\"camera\": { \"fps\": 30 ";  // 截断 JSON；内容独特，便于逐字节比对
    if (!writeAllText(cfgPath, bad)) return false;

    FileId before{};                                // 隔离前原文件的身份
    if (!fileIdOf(cfgPath, before)) return false;   // 构造不出场景则响亮失败，绝不静默通过

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    const auto corrupts = listCorruptConfigs(dir);
    if (corrupts.size() != 1) return false;         // 隔离成功：恰好一个 corrupt 产物

    // (1) 命名格式：config.json.corrupt.<十进制毫秒>
    const std::string name = corrupts[0].filename().string();
    const std::string prefix = "config.json.corrupt.";
    if (name.rfind(prefix, 0) != 0) return false;
    const std::string suffix = name.substr(prefix.size());
    if (suffix.size() < 12) return false;           // 位数不足 ⇒ 很可能是秒级时间戳
    for (const char c : suffix) {
        if (c < '0' || c > '9') return false;       // 非纯十进制（秒/其它后缀）⇒ 红
    }

    // 内容逐字节保全（移动与拷贝都满足，仅证明证据完整，不作判别）。
    std::string qtext;
    if (!readAllText(corrupts[0], qtext) || qtext != bad) return false;

    // (2) 移动判据：隔离产物与原文件是**同一个文件**（唯一标识不变）。
    FileId after{};
    if (!fileIdOf(corrupts[0], after)) return false;
    return sameFileId(before, after);
}

// FACE-CONFIG-006（时间戳冲突重试）：候选名全部被占满 ⇒ 隔离失败并**保留原文件**。
//
// 构造（自适应 + 封顶，替代早期「固定预建 8192 个」的做法）：
//   quarantineConfigFile 以「调用时刻的毫秒 ms」为基准，尝试 [ms, ms+99] 共 100 个连号候选名。
//   我们先在 [t0, t0+N-1] 预建占位文件（t0 = 预建前测得的毫秒）。因 ms ≥ t0 + elapsed（elapsed = 预建
//   耗时），窗口右端 ms+99 ≤ t0 + elapsed + 99；只要 N ≥ elapsed + 200，就有 t0+N-1 ≥ t0+elapsed+199
//   ≥ ms+99 ⇒ 候选窗被完全覆盖 ⇒ 隔离必然失败。判据**自适应**：循环持续预建直到 N ≥ elapsed + 200
//   （每 64 个文件重测一次 elapsed），不依赖任何「预建耗时 < 固定窗口」的固定假设。
//   （订正：早前 8192 个固定占位 + 「余量数百倍」的说法不实——实际余量仅约 82×（8192/99），且加宽固定
//    窗口会同时抬高预建成本、比值不变，慢盘/杀软下仍会偶发红。故改为自适应判据。）
//   封顶：若磁盘极慢 / 杀软逐文件扫描（每文件 ≈1ms ⇒ elapsed ≈ N，判据永不满足），到 N > 200000 或
//   elapsed > 60s 即停止并**跳过**（打一行 stderr 说明），而非判失败——这是「构造不出场景」而不是
//   「被测行为错误」，静默通过或偶发红都比显式跳过更糟。
//
// 判别力（订正早前的不实描述）：
//   · 若**去掉** quarantineConfigFile 的 `exists(dst) → continue` 预探（本平台 rename 会**覆盖**已存在
//     目标），attempt=0 会覆盖某个占位 ⇒ 隔离「成功」⇒ initQuarantineFileName() 非空 ⇒ 用例变红。
//   · 反之，**仅**把取名恒定为 attempt=0 而**保留**预探时，`exists(dst)` 恒真 ⇒ 仍穷尽 100 次后失败、
//     用例**仍绿**。故本用例判的是「预探 + 连号重试」的组合，早前「恒用 attempt=0 会让用例变红」不成立。
//   · 同时要求原文件逐字节未被覆盖（隔离失败 ⇒ 不落盘默认值；「不被就地覆盖」仅限本次装载，见源码注释）。
bool test_config_quarantine_timestamp_collision_preserves_original() {
    const auto dir = makeTempDir("quarantine_collision");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    const std::string bad = "{\"camera\": { \"fps\": 30 ";
    if (!writeAllText(cfgPath, bad)) return false;

    const auto nowMs = [] {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    };
    const std::uint64_t t0 = nowMs();
    std::uint64_t created = 0;
    std::uint64_t elapsed = 0;
    bool capped = false;
    for (;;) {
        if (created > 200000 || elapsed > 60000) { capped = true; break; }
        if (created >= elapsed + 200) break;  // 已覆盖候选窗且留 200ms 余量
        if (!writeAllText(dir / ("config.json.corrupt." + std::to_string(t0 + created)), "")) return false;
        ++created;
        if ((created & 63) == 0) elapsed = nowMs() - t0;  // 周期性重测，避免每文件一次 syscall
    }
    if (capped) {
        std::cerr << "[skip] test_config_quarantine_timestamp_collision_preserves_original: 场景未能构造"
                     "（占位未达覆盖窗内 200ms 余量：N=" << created << ", elapsed=" << elapsed << "ms），本用例跳过"
                  << std::endl;
        return true;  // 构造不出场景 ⇒ 跳过，不判失败（见上方封顶说明）
    }

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    // 覆盖性复核（防偶发红）：隔离调用时刻 ms ≤ tAfter（本行测得）。若 tAfter+99 ≤ t0+N-1 仍成立，则
    // 候选窗 [ms, ms+99] 必被 [t0, t0+N-1] 完整覆盖（因 ms ≤ tAfter ⇒ ms+99 ≤ tAfter+99 ≤ t0+N-1）⇒
    // 隔离**必然**失败。若此条不成立（占位期间/初始化出现 >100ms 级停顿），场景未能构造 ⇒ 跳过而非判失败。
    const std::uint64_t tAfter = nowMs();
    if (tAfter + 100 > t0 + created) {
        std::cerr << "[skip] test_config_quarantine_timestamp_collision_preserves_original: 场景未能构造"
                     "（预建跨度未覆盖调用时刻候选窗：t0+created=" << (t0 + created) << ", tAfter=" << tAfter
                  << "），本用例跳过" << std::endl;
        return true;
    }

    // 隔离必然失败：无隔离文件名、告警含失败原因、原文件未被覆盖。
    if (!store.configRejectedOnLoad()) return false;
    if (!store.initQuarantineFileName().empty()) return false;  // 覆盖了占位 ⇒ 红
    if (warn.find("隔离失败") == std::string::npos) return false;
    std::string after;
    if (!readAllText(cfgPath, after)) return false;
    if (after != bad) return false;  // ★ 原文件逐字节未被动过
    return true;
}

// FACE-CONFIG-006 补充：隔离**重试成功**路径（上方 collision 用例只压「穷尽失败」路径）。
//
// 构造：占位候选窗的**前若干个**名字 [t0, t0+K]（t0 = 占位前测得的毫秒），使 attempt=0..K 落空、
//   下一个空位（= t0+K+1）命中 ⇒ 隔离成功，产物名应恰为 t0+K+1（即 ms + attempt，attempt = t0+K+1−ms ≥ 1）。
// 判据为何成立：quarantine 的 ms 取自其调用时刻（≥ t0）；若「占位 → pollReloadOnce」全程在 K 毫秒内
//   完成（tAfter ≤ t0+K），则 ms ≤ tAfter ≤ t0+K ⇒ 候选 [ms, ms+99] 中被占位的恰为 [ms, t0+K]，
//   首个空位 = t0+K+1 ⇒ 产物后缀必为 t0+K+1 且 attempt ≥ 1（attempt=0 的产物名会落在被占位区间内）。
// 约束：若超出 K 毫秒（慢盘 / 杀软），场景未能构造 ⇒ 打一行 stderr 说明并**跳过**（不偶发失败）。
bool test_config_quarantine_retry_succeeds_after_collisions() {
    const auto dir = makeTempDir("quarantine_retry_success");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 先 initialize 落地默认配置（同时生成密钥），使后面 pollReloadOnce 只做轻量的 reload，
    // 不把 DPAPI 初始化耗时计入 K 窗口。
    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (std::filesystem::exists(dir / "config.json.bak")) return false;  // 前置条件：无 .bak（fresh 目录）

    // 外部把 config.json 写成坏内容并拨快 mtime。
    const std::string bad = "{\"camera\": { \"fps\": 30 ";
    if (!writeAllText(cfgPath, bad)) return false;
    if (!bumpMtimePlus2s(cfgPath)) return false;

    const auto nowMs = [] {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    };
    const std::uint64_t K = 50;
    const std::uint64_t t0 = nowMs();
    for (std::uint64_t i = 0; i <= K; ++i) {
        if (!writeAllText(dir / ("config.json.corrupt." + std::to_string(t0 + i)), "")) return false;
    }

    bool applied = true;
    std::string err;
    store.pollReloadOnce(applied, err);
    const std::uint64_t tAfter = nowMs();
    if (tAfter > t0 + K) {
        std::cerr << "[skip] test_config_quarantine_retry_succeeds_after_collisions: 场景未能构造"
                     "（占位 + poll 超出 K=" << K << "ms 窗口），本用例跳过" << std::endl;
        return true;
    }

    if (applied) return false;  // 被拒 ⇒ 不应用
    // 隔离成功：产物名 = t0+K+1（首个未占位候选 = ms + attempt，attempt ≥ 1）。
    const std::uint64_t expect = t0 + K + 1;
    const auto corrupts = listCorruptConfigs(dir);
    if (corrupts.size() != (K + 2)) return false;  // K+1 个占位 + 1 个隔离产物
    for (const auto& p : corrupts) {
        if (p.filename().string() == "config.json.corrupt." + std::to_string(expect)) {
            std::string txt;
            if (!readAllText(p, txt) || txt != bad) return false;  // 逐字节保全
            return true;
        }
    }
    return false;  // 未命中预期名 ⇒ 重试/取名逻辑偏离
}

// FACE-CONFIG-001 补充：热重载被拒但**无可用 .bak** ⇒ 只隔离、不写盘（config.json 不存在）。
// 补齐此前只覆盖启动路径的空白：热重载的隔离 + 「无 .bak 不写盘」语义（config.json 早退）。
bool test_config_reload_rejected_no_bak_quarantines_without_write() {
    const auto dir = makeTempDir("reload_reject_no_bak");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (std::filesystem::exists(dir / "config.json.bak")) return false;  // 前置条件防呆

    const std::string bad = "{\"camera\": { \"fps\": 30 ";
    if (!writeAllText(cfgPath, bad)) return false;
    if (!bumpMtimePlus2s(cfgPath)) return false;

    bool applied = true;
    std::string err;
    store.pollReloadOnce(applied, err);

    if (applied) return false;
    // 原文件被隔离（逐字节保全）。
    const auto corrupts = listCorruptConfigs(dir);
    if (corrupts.size() != 1) return false;
    std::string qtext;
    if (!readAllText(corrupts[0], qtext) || qtext != bad) return false;
    // config.json 不存在（无 .bak ⇒ 不写盘）；无回滚临时文件残留。
    if (std::filesystem::exists(cfgPath)) return false;
    if (std::filesystem::exists(dir / "config.json.rollback.tmp")) return false;
    return true;
}

// FACE-CONFIG-001 补充（**核心**）：热重载被拒 ⇒ 隔离 + 保全证据 + **不应用**。
//
// 判别力：这是本轮「回退改动前语义」的行为钉。构造使 .bak 合法但比内存配置**更旧**：
//   initialize 落地默认配置（width=640）→ PUT 改 width=800（此后 .bak = width=640 的旧配置）
//   → 外部把 config.json 改成**坏内容**。修复前（rolledBack 跳过守卫）：隔离后采纳 .bak ⇒
//   cfg_.camera.width 变回 640 且 generation+1（经 win_local_service_main 触发管线 FullRestart）
//   ——即一次无关的外部坏写静默撤销用户最近一次 PUT。修复后：隔离坏文件、把 .bak 写回 config.json，
//   但内存仍为 width=800、generation 不变、outApplied=false。本用例对每一环分别断言。
bool test_config_reload_rejected_quarantines_and_not_applied() {
    const auto dir = makeTempDir("reload_reject_not_applied");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 1) 落地默认配置，再 PUT 把 width 改成非默认 800（同时把旧配置备份进 .bak）。
    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (!store.updateFromJsonBody("{\"camera\":{\"width\":800}}").ok) return false;
    if (store.current().camera.width != 800) return false;
    if (!std::filesystem::exists(dir / "config.json.bak")) return false;  // 前置条件：.bak 存在才有判别力
    const std::uint64_t gen0 = store.generation();

    // 2) 外部把 config.json 改成坏内容（JSON 可解析但被 schema 拒：minFaceSizePx 期望 number）。
    std::string base;
    if (!readAllText(cfgPath, base)) return false;
    rk_win::JsonValue doc;
    std::string perr;
    if (!rk_win::parseJson(base, doc, perr) || !doc.isObject()) return false;
    rk_win::JsonValue* rec = doc.find("recognition");
    if (!rec || !rec->isObject()) return false;
    rec->o["minFaceSizePx"] = rk_win::JsonValue::makeString("80");
    const std::string bad = rk_win::toJsonString(doc, true);
    if (!writeAllText(cfgPath, bad)) return false;
    if (!bumpMtimePlus2s(cfgPath)) return false;

    // 3) 热重载：必须被拒（不应用）。
    bool applied = true;
    std::string err;
    store.pollReloadOnce(applied, err);

    // (a) 目录出现恰好一个隔离产物。
    const auto corrupts = listCorruptConfigs(dir);
    if (corrupts.size() != 1) return false;
    // (b) 隔离文件与坏内容逐字节相同（保全证据，而非删除重建）。
    std::string qtext;
    if (!readAllText(corrupts[0], qtext)) return false;
    if (qtext != bad) return false;
    // (c) 未应用且代数未变（「不撤销最近一次 PUT / 不触发管线重启」的判据）。
    if (applied) return false;
    if (store.generation() != gen0) return false;
    // (d) 内存配置仍是原来那份（width=800；修复前会被 .bak 回滚成 640）。
    if (store.current().camera.width != 800) return false;
    return true;
}

// FACE-CONFIG-002：默认配置**序列化产物未通过 schema**（密钥不可用）时拒绝落盘 stub。
//
// 构造（确定性，不依赖墙钟/竞态）：预写 config.key.dpapi 为存在但 base64 非法的内容 ⇒
//   ensureOrLoadKey 的 base64Decode 失败（即便侥幸解码成功，dpapiUnprotect 也对垃圾字节失败）⇒
//   密钥不可用 ⇒ buildSettingsJson(..., encryptSensitive=true) 返回 {"schemaVersion":1,"_error":...} stub
//   ⇒ serializedDocIsWritable 拒之 ⇒ 不落盘。同时**不放** config.json。
bool test_config_missing_key_rejects_default_write() {
    const auto dir = makeTempDir("gate_key_unavailable");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 存在但 base64 非法的密钥文件；不写 config.json。
    if (!writeAllText(dir / "config.key.dpapi", "!!!not-base64!!!")) return false;

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    // (a) 拒绝把 stub 落盘 ⇒ config.json 不存在。
    if (std::filesystem::exists(cfgPath)) return false;
    // (b) 失败要有可见性：warn 含拒写原因。
    if (warn.find("序列化未通过 schema") == std::string::npos) return false;
    // (c) 本轮并无「既有配置被拒」（config.json 本就不存在）。
    if (store.configRejectedOnLoad()) return false;
    return true;
}

// FACE-CONFIG-004：同一 store 二次 initialize 必须复位上一轮的启动诊断。
// 同一 store 先装载坏配置（⇒ rejected + 隔离），再装载好配置 ⇒ 三个诊断字段全部复位。
bool test_config_second_initialize_resets_diagnostics() {
    const auto dir = makeTempDir("second_init_reset");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 1) 坏配置（截断 JSON）⇒ 首次 initialize 判为 rejected 且发生隔离。
    const std::string bad = "{\"camera\": { \"fps\": 30 ";
    if (!writeAllText(cfgPath, bad)) return false;

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (!store.configRejectedOnLoad()) return false;
    if (store.initQuarantineFileName().empty()) return false;

    // 2) 二次 initialize：config.json 已被首轮以默认值重新落地 ⇒ 装载成功，三个诊断字段必须复位。
    store.initialize(warn);
    if (store.configRejectedOnLoad()) return false;
    if (!store.initWarning().empty()) return false;
    if (!store.initQuarantineFileName().empty()) return false;
    return true;
}

// FACE-CONFIG 行为级收束：运行时**真的拒绝** schema 不允许的未知键（additionalProperties:false）。
//
// 背景：config-contract.test.js 仅查 schema **文本声明**，删掉 JsonSchemaValidator.cpp 的强制块（探针
//   在 validateSchemaRec 内）它仍绿。本用例钉住**行为**：
//   (1) PUT 路径（parseAndValidateSettingsDoc 的可达入口，间接经 updateFromJsonBody 的 schema 校验）：
//       给 camera 段加 schema 不允许的键、顶层加未知段，均必须被拒（res.ok == false）；
//   (2) 直接对运行时 schema 喂「合法默认文档 + 顶层未知键」⇒ validate 必须返回 false 且 err 非空。
//   任一强制块被删 ⇒ 未知键被收下 ⇒ 本用例变红。
bool test_config_unknown_key_rejected() {
    const auto dir = makeTempDir("unknown_key_reject");
    ConfigEnvGuard env(dir / "config.json");

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    // (1a) camera 段未知键。
    if (store.updateFromJsonBody("{\"camera\":{\"bogusUnknownKey\":1}}").ok) return false;
    // (1b) 顶层未知段。
    if (store.updateFromJsonBody("{\"bogusTopLevel\":{}}").ok) return false;

    // (2) 直接对运行时 schema 喂合法默认文档 + 未知键。
    const std::string json = store.currentRedactedJsonPretty();
    rk_win::JsonValue doc;
    std::string perr;
    if (!rk_win::parseJson(json, doc, perr) || !doc.isObject()) return false;
    doc.o["bogusUnknownKey"] = rk_win::JsonValue::makeNumber(1);
    std::string schemaErr;
    if (rk_win::JsonSchemaValidator::validate(
            rk_win::JsonSchemaValidator::buildSettingsDocSchema(), doc, &schemaErr)) {
        return false;  // 未知键被接受 ⇒ 收束未生效
    }
    return !schemaErr.empty();  // 失败须可见
}
