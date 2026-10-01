#include "rk_win/WinJsonConfig.h"
#include "rk_win/JsonLite.h"
#include "rk_win/JsonSchemaValidator.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <io.h>  // MSVC: _dup/_dup2/_close/_fileno（FACE-CONFIG-012 stderr 重定向捕获）
#include <iostream>
#include <string>
#include <thread>
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

// FACE-CONFIG-012：把 stderr（含 std::cerr）重定向到文件以捕获 initialize 的独立可见通道输出。
// 捕获手段**照抄** tests/win/test_frame_pipeline_reload.cpp 的 _dup/_dup2/freopen 三连（MSVC fd 语义）：
//   fflush(stdout)/fflush(stderr)/cerr.flush() → _dup 保存原 fd → freopen 指向捕获文件 → 收尾 _dup2 还原。
// 与先例的差异（本文件是 return-false 风格、存在大量提前 return）：改用 **RAII** 守卫，保证任何提前
//   return（甚至中途断言失败）都还原 stderr——否则 fd 2 会永久指向捕获文件，后续用例输出全部丢失。
struct StderrCapture {
    int savedFd = -1;
    bool active = false;
    std::filesystem::path path;

    explicit StderrCapture(const std::filesystem::path& p) : path(p) {
        fflush(stdout);
        fflush(stderr);
        std::cerr.flush();
        savedFd = _dup(_fileno(stderr));
        if (savedFd < 0) return;
        if (freopen(path.string().c_str(), "w", stderr) == nullptr) {
            _close(savedFd);
            savedFd = -1;
            return;
        }
        active = true;
    }
    // 结束捕获：flush 后还原 stderr，再读回捕获文本（空串 = 捕获不可用或确无输出）。
    std::string finish() {
        if (!active) return std::string();
        fflush(stderr);
        std::cerr.flush();
        _dup2(savedFd, _fileno(stderr));
        _close(savedFd);
        savedFd = -1;
        active = false;
        std::string out;
        std::ifstream in(path, std::ios::binary);
        if (in.is_open()) out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return out;
    }
    ~StderrCapture() {
        if (!active) return;
        // 兜底还原（用例未调 finish 就提前 return 时）。
        fflush(stderr);
        std::cerr.flush();
        _dup2(savedFd, _fileno(stderr));
        _close(savedFd);
    }
};

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

// ★ 2026-09-28 新增（外部审查 [P2]「一次临时读取失败会使热重载停止重试」的**判别用例**）。
// 缺陷：`reloadFromDisk` 原先在 `readFileAll` 失败时**记录该文件的 mtime** ⇒ 之后每次轮询都在
//   `lastWriteTime_ == wt` 处早退。而「可读性恢复」这件事在 Windows 上**不会**更新 LastWriteTime
//   （解除独占占用、放宽 ACL 都只动 ChangeTime）⇒ 文件恢复可读后仍被**永久跳过**，配置一直陈旧到
//   下一次真实写入或重启，且没有任何用户可见提示。
// 修复：读取失败**不**标记已处理，下次轮询（≤1s）重试。
//
// 判别力（**确定性**，不依赖线程交错、不依赖独占句柄）：要让「旧实现红、新实现绿」，必须构造
//   「可读性恢复、但 mtime 未变」——做法是先用**超上限**的文件让读取失败（该分支填 err 且不碰
//   mtime、且 `last_write_time` 照常可取），再把文件改回合法大小，并把 mtime **精确设回**失败时
//   观察到的值。旧实现已把 lastWriteTime_ 记成该值 ⇒ 相等早退 ⇒ `applied2` 为 false（红）；
//   新实现未记 ⇒ 正常读入并应用（绿）。
// 前置锚（两处，均**响亮 return false**，绝不静默跳过）：① 第一次轮询必须失败**且带 err**
//   （否则可能只是 mtime 查询失败那条早退，等于没测到目标分支）；② 设回的 mtime 必须与 t1
//   **精确相等**（否则旧实现也会重试 ⇒ 本用例失去判别力，会退化成恒真）。
//
// ⚠️ **已知「误红面」（对抗性核查指出，如实登记，不视为缺陷但改动本用例前必须一并考虑）**：
//   ① 锚 ① 与实现常量**耦合**：超限内容 = `good + 11 MiB 空白`，而判据是
//      `kMaxConfigFileBytes`（当前 10 MiB）。**若有人把上限抬到 ≥ 11 MiB**，`readFileAll` 就会成功、
//      `applied1` 为 true ⇒ 本用例在**正确实现**上变红。⇒ 届时须同步把这里的 11 MiB 调大。
//   ② 锚 ② 对**卷的时间戳粒度**敏感：`makeTempDir` 用的是 `temp_directory_path()`；若落在
//      FAT/exFAT（2s 粒度）或某些重定向的 `%TEMP%` 上，`last_write_time(path, t1)` 会被取整 ⇒
//      `t1back != t1` ⇒ 同样在**正确实现**上变红。⇒ 本机（NTFS）实测精确，但换环境需留意。
//   ③ 同族的构造性依赖：`bumpMtimePlus2s` 失败 / 11 MiB 写入因配额或磁盘满失败 ⇒ 也是红（非静默）。
bool test_config_reload_retries_after_read_failure_without_mtime_change() {
    const auto dir = makeTempDir("reload_read_retry");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    // 留一份 initialize 产出的**合法**配置原文，供第 2 步原样写回（保证 schema 校验通过，
    // 避免把「内容非法 ⇒ 走隔离」误当成「重试成功」）。
    std::string good;
    if (!readAllText(cfgPath, good)) return false;
    if (good.empty()) return false;

    // 第 1 步：写入超上限内容（走 readFileAll 的「文件超过大小上限」分支）。
    std::string oversized = good;
    oversized.append(11u * 1024u * 1024u, ' ');  // 11MB > kMaxConfigFileBytes(10MB)
    if (!writeAllText(cfgPath, oversized)) return false;
    if (!bumpMtimePlus2s(cfgPath)) return false;

    std::error_code ec;
    const auto t1 = std::filesystem::last_write_time(cfgPath, ec);
    if (ec) return false;

    bool applied1 = true;
    std::string err;
    store.pollReloadOnce(applied1, err);
    if (applied1) return false;    // 前置锚 ①-a：必须先失败
    if (err.empty()) return false; // 前置锚 ①-b：且必须**带 err**（区别于 mtime 查询失败的静默早退）

    // 第 2 步：恢复可读性，但把 mtime 精确设回 t1 —— 这正是缺陷的前提。
    if (!writeAllText(cfgPath, good)) return false;
    std::filesystem::last_write_time(cfgPath, t1, ec);
    if (ec) return false;
    std::error_code ecChk;
    const auto t1back = std::filesystem::last_write_time(cfgPath, ecChk);
    if (ecChk || t1back != t1) return false;  // 前置锚 ②：设回必须精确，否则本用例无判别力

    // 第 3 步：★ 核心断言（旧实现必红）——可读性恢复后必须能重载。
    bool applied2 = false;
    store.pollReloadOnce(applied2, err);
    if (!applied2) return false;
    return true;
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

// 清空 postUrl 的**端到端**完备性（「经 UI 清空」这一修复的后端承重路径，此前**零覆盖**）。
//
// 为什么必须有这条：前端「把 "***" 删空并保存 = 显式清空」能成立，**前提是后端接受并持久化空串**。
//   空串在这条链上要连过四关，任一关被改坏都会让「清空」静默失效甚至更糟：
//     ① updateFromJsonBody 的 stripMaskedSentinels **只剔精确 "***"、不剔 ""**（若哪天连空串也剔，
//        语义就变成"不修改" ⇒ 清空永久不生效，且界面仍报保存成功）；
//     ② schema 的 poster.postUrl 串分支**无 minLength / pattern**（JsonSchemaValidator 也未实现
//        minLength）⇒ "" 通过校验；
//     ③ **空明文加解密往返**：encryptSensitive 分支对空串加密 ⇒ base64Encode(空缓冲) 显式早退返回
//        空 ciphertext ⇒ 解密侧只守 nonce/tag、不守 ciphertext ⇒ 还原为空明文。
//        ⚠️ 这一步在 MSVC/BCrypt 上搭了「pbInput 与 pbOutput **同为 NULL** ⇒ 按零长度认证加密处理」
//        这一**特例**的便车（空 vector 的 data() 为 nullptr）。若该特例不成立，落盘 tag 会是全零 ⇒
//        重启解密失败 ⇒ 整份 config 被隔离（FACE-CONFIG-001）⇒ **清空被静默撤销**并回落默认 URL。
//        故本用例是这条特例的**唯一**实测判据。
//     ④ 装载侧对 postUrl 用 `find("postUrl")` 的**存在性**判据（不是 getString 的缺失回退）⇒ 键存在
//        且为空串时**保留空串**，不会被 defaultAppConfig 的默认 URL 顶回。
//   ③ 最危险且全仓此前没有任何用例触达：默认 postUrl 非空（WinConfig.h 的默认值），既有的 roundtrip /
//   masked-echo 用例都不写空 postUrl，`tests/` 也从不直接测 aes256gcm*。
// 判别点：① 若 strip 改成也剔空串 / ③ 若给加密加空输入早退 / ④ 若装载侧改成"空串视为缺失"，
//   下方对应断言即变红；把 PUT 改成不落盘同样红（重载实例会看到默认 URL）。
bool test_config_empty_post_url_round_trips_and_survives_reload() {
    SetEnvironmentVariableW(L"RK_WCFR_DNN_MODEL", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_DNN_CONFIG", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_HTTP_PORT", nullptr);
    // 该 env 会在装载时**覆盖** postUrl（WinJsonConfig 的冗余 env 覆盖块），必须清掉否则本用例假红。
    SetEnvironmentVariableW(L"RK_WCFR_POST_URL", nullptr);

    const auto dir = makeTempDir("empty_posturl");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    const std::string realUrl = "https://real.example/api/faces";
    const auto resSet = store.updateFromJsonBody("{\"poster\":{\"postUrl\":\"" + realUrl + "\"}}");
    if (!resSet.ok) return false;
    if (store.current().poster.postUrl != realUrl) return false;

    // ① + ③：PUT 显式清空（这正是 UI 从「删空 "***"」发出的形状——哨兵在前端已被折算掉，补丁里就是空串）。
    // 同时改一个**可判别**的同段字段：若落盘文件被损坏到无法装载、或以默认值启动，throttleMs 会是默认
    // 100 而非 1234 ⇒ 下面那条断言即可区分「读到了同一份文件」与「整体回落默认值」。
    const auto resClear = store.updateFromJsonBody("{\"poster\":{\"postUrl\":\"\",\"throttleMs\":1234}}");
    if (!resClear.ok) return false;
    if (!store.current().poster.postUrl.empty()) {
        std::cerr << "[empty_posturl] PUT 空串后 current().poster.postUrl 非空（长度 "
                  << store.current().poster.postUrl.size() << "）" << std::endl;
        return false;
    }
    if (!std::filesystem::exists(cfgPath)) return false;

    // ② + ④：**新实例**读同一份落盘 config.json ⇒ 完整覆盖「空明文加密 → base64Encode(空) → 解密还原」
    // 与「键存在且为空串 ⇒ 保留空串」。
    rk_win::WinJsonConfigStore reloaded;
    std::string warn2;
    reloaded.initialize(warn2);
    if (!reloaded.current().poster.postUrl.empty()) {
        std::cerr << "[empty_posturl] 重载后 postUrl 非空（长度 "
                  << reloaded.current().poster.postUrl.size()
                  << "）——空明文往返失败，或空串被当成缺失而回落默认值/旧值" << std::endl;
        return false;
    }
    if (reloaded.current().poster.throttleMs != 1234) {
        std::cerr << "[empty_posturl] 重载后 throttleMs=" << reloaded.current().poster.throttleMs
                  << "（期望 1234）——落盘文件未生效或被拒后以默认值启动" << std::endl;
        return false;
    }
    return true;
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

// FACE-CONFIG-001 / FACE-CONFIG-010 补充：热重载被拒且**无可用 .bak** ⇒ 用内存中当前生效的配置写回。
// （FACE-CONFIG-010 之前：无 .bak 即不写盘，config.json 消失、磁盘与内存脱节；现在即便无 .bak 也把内存
//   配置写回，磁盘与内存保持一致。）
bool test_config_reload_rejected_no_bak_rewrites_from_memory() {
    const auto dir = makeTempDir("reload_reject_no_bak");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (std::filesystem::exists(dir / "config.json.bak")) return false;  // 前置条件防呆：无 .bak
    const std::uint64_t gen0 = store.generation();

    const std::string bad = "{\"camera\": { \"fps\": 30 ";
    if (!writeAllText(cfgPath, bad)) return false;
    if (!bumpMtimePlus2s(cfgPath)) return false;

    bool applied = true;
    std::string err;
    store.pollReloadOnce(applied, err);

    if (applied) return false;
    if (store.generation() != gen0) return false;  // 不应用：代数不变
    // 原文件被隔离（逐字节保全）。
    const auto corrupts = listCorruptConfigs(dir);
    if (corrupts.size() != 1) return false;
    std::string qtext;
    if (!readAllText(corrupts[0], qtext) || qtext != bad) return false;
    // FACE-CONFIG-010：无 .bak 也写回内存配置 ⇒ config.json 存在且是一份合法 settings 文档。
    if (!std::filesystem::exists(cfgPath)) return false;
    std::string back;
    if (!readAllText(cfgPath, back)) return false;
    if (back == bad) return false;  // 不得把坏内容原地留回
    rk_win::JsonValue backDoc;
    std::string berr;
    if (!rk_win::parseJson(back, backDoc, berr) || !backDoc.isObject()) return false;
    std::string schemaErr;
    if (!rk_win::JsonSchemaValidator::validate(
            rk_win::JsonSchemaValidator::buildSettingsDocSchema(), backDoc, &schemaErr)) {
        return false;  // 写回的必须是可再次装载的合法配置（而非 _error stub / 坏内容）
    }
    // 无回滚临时文件残留。
    if (std::filesystem::exists(dir / "config.json.rollback.tmp")) return false;
    return true;
}

// FACE-CONFIG-010（**核心判别**）：热重载被拒时写回 config.json 的必须是**内存中当前生效的配置**，
// 而非可能更旧的 .bak（.bak 是「上一次写入前」的副本，会静默回退用户最近一次成功 PUT）。
// 构造：initialize 落地默认（width=640）→ PUT width=800（此后 .bak = width 640 的旧配置）→ 外部把
//   config.json 改成坏内容 → pollReloadOnce 被拒。修复前写回 .bak ⇒ 磁盘 width=640；修复后写回内存
//   ⇒ 磁盘 width=800。本用例对磁盘内容直接断言，红在「回滚源用 .bak」这一改法上。
bool test_config_reload_rejected_rewrites_memory_not_stale_bak() {
    const auto dir = makeTempDir("reload_reject_memory_not_bak");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (!store.updateFromJsonBody("{\"camera\":{\"width\":800}}").ok) return false;
    if (store.current().camera.width != 800) return false;
    if (!std::filesystem::exists(dir / "config.json.bak")) return false;  // 前置条件：.bak 存在才有判别力
    const std::uint64_t gen0 = store.generation();

    // 坏内容（JSON 可解析但被 schema 拒：minFaceSizePx 期望 number）。
    std::string base;
    if (!readAllText(cfgPath, base)) return false;
    rk_win::JsonValue doc;
    std::string perr;
    if (!rk_win::parseJson(base, doc, perr) || !doc.isObject()) return false;
    rk_win::JsonValue* rec = doc.find("recognition");
    if (!rec || !rec->isObject()) return false;
    rec->o["minFaceSizePx"] = rk_win::JsonValue::makeString("80");
    if (!writeAllText(cfgPath, rk_win::toJsonString(doc, true))) return false;
    if (!bumpMtimePlus2s(cfgPath)) return false;

    bool applied = true;
    std::string err;
    store.pollReloadOnce(applied, err);
    if (applied) return false;
    if (store.generation() != gen0) return false;
    if (store.current().camera.width != 800) return false;  // 不应用：内存仍为 PUT 后的 800

    // ★ 写回的 config.json 必须是内存配置（width=800），而非 .bak（width=640）。
    std::string back;
    if (!readAllText(cfgPath, back)) return false;
    rk_win::JsonValue backDoc;
    std::string berr;
    if (!rk_win::parseJson(back, backDoc, berr) || !backDoc.isObject()) return false;
    const rk_win::JsonValue* cam = backDoc.find("camera");
    if (!cam || !cam->isObject()) return false;
    const rk_win::JsonValue* wv = cam->find("width");
    if (!wv || !wv->isNumber()) return false;
    return wv->n == 800;  // 修复前 = 640（旧 .bak）⇒ 红
}

// FACE-CONFIG-001 补充（**核心**）：热重载被拒 ⇒ 隔离 + 保全证据 + **不应用**。
//
// 判别力：这是本轮「回退改动前语义」的行为钉。构造使 .bak 合法但比内存配置**更旧**：
//   initialize 落地默认配置（width=640）→ PUT 改 width=800（此后 .bak = width=640 的旧配置）
//   → 外部把 config.json 改成**坏内容**。修复前（rolledBack 跳过守卫）：隔离后采纳 .bak ⇒
//   cfg_.camera.width 变回 640 且 generation+1（经 win_local_service_main 触发管线 FullRestart）
//   ——即一次无关的外部坏写静默撤销用户最近一次 PUT。修复后：隔离坏文件、把**内存中当前生效的配置**
//   （width=800）写回 config.json（FACE-CONFIG-010：不再用可能更旧的 .bak），但内存仍为 width=800、
//   generation 不变、outApplied=false。本用例对每一环分别断言。
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

// ②（对抗审查收口）：**密钥不可用**时热重载被拒 ⇒ 内存与 .bak **两臂皆不可用** ⇒ 不写盘。
// 判据：.bak 臂先经 parseAndValidateSettingsDoc，而该函数**成功路径必经** ensureOrLoadKey；密钥不可用
//   时 .bak 会被同判「也无效」⇒ 落到「.bak 也无效」臂（本用例的断言即要求 outErr 含该文案）。故「密钥不可用」**不会**回退 .bak、也不会把内存
//   stub 写回——而是两者都不写。修复前的注释/文档把「密钥不可用」当作回退 .bak 的场景，属失实（本用例
//   给出可判别证据：outErr 必须含「.bak 也无效」，且不得发生写回）。
bool test_config_reload_rejected_key_unavailable_bak_unusable() {
    const auto dir = makeTempDir("reload_key_bad_bak_unusable");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 1) 先落盘一份合法配置（含密钥），并把 config.json 复制为 .bak（schema-valid 且以当前密钥加密）。
    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (!std::filesystem::exists(cfgPath)) return false;
    {
        std::string t;
        if (!readAllText(cfgPath, t)) return false;
        if (!writeAllText(dir / "config.json.bak", t)) return false;
    }
    const std::uint64_t gen0 = store.generation();

    // 2) 破坏密钥：此后 buildSettingsJson 与 parseAndValidateSettingsDoc 都取不到可用密钥。
    if (!writeAllText(dir / "config.key.dpapi", "!!!not-base64!!!")) return false;

    // 3) 外部把 config.json 改成坏内容（截断 JSON）。
    const std::string bad = "{\"camera\": { \"fps\": 30 ";
    if (!writeAllText(cfgPath, bad)) return false;
    if (!bumpMtimePlus2s(cfgPath)) return false;

    bool applied = true;
    std::string err;
    store.pollReloadOnce(applied, err);

    if (applied) return false;
    if (store.generation() != gen0) return false;             // 不应用：代数不变
    // ★ 判别断言：.bak 因同一把密钥不可用而被判无效（而非「回退 .bak 成功」）。
    if (err.find(".bak 也无效") == std::string::npos) return false;
    // 两臂皆不可用 ⇒ 不写盘：config.json 已被隔离 ⇒ 不存在。
    if (std::filesystem::exists(cfgPath)) return false;
    const auto corrupts = listCorruptConfigs(dir);
    if (corrupts.size() != 1) return false;
    std::string qtext;
    if (!readAllText(corrupts[0], qtext) || qtext != bad) return false;
    if (std::filesystem::exists(dir / "config.json.rollback.tmp")) return false;  // 无临时残留
    return true;
}

// ③（对抗审查收口）：密钥不可用 + 无 .bak ⇒ 不写盘；config.json 已隔离 ⇒ **不存在** ⇒ 下次启动进入
//   「不存在」分支 ⇒ 不判 rejected、以默认值运行（密钥也坏 ⇒ degraded 可见）⇒ **用户原配置永不自动恢复**，
//   需人工把 config.json.corrupt.* 改名回 config.json。本用例把「不写盘 → 下次启动不 rejected」这一后果
//   端到端钉住（判别力：若回滚改成「无 .bak 也写默认/写内存」，config.json 会存在且本用例的 not-exist 断言红）。
bool test_config_reload_rejected_key_unavailable_no_bak_no_selfheal() {
    const auto dir = makeTempDir("reload_key_bad_no_bak");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (std::filesystem::exists(dir / "config.json.bak")) return false;  // 前置：确无 .bak
    const std::uint64_t gen0 = store.generation();

    if (!writeAllText(dir / "config.key.dpapi", "!!!not-base64!!!")) return false;
    const std::string bad = "{\"camera\": { \"fps\": 30 ";
    if (!writeAllText(cfgPath, bad)) return false;
    if (!bumpMtimePlus2s(cfgPath)) return false;

    bool applied = true;
    std::string err;
    store.pollReloadOnce(applied, err);

    if (applied) return false;
    if (store.generation() != gen0) return false;
    // ★ 走「无可用 .bak」臂，并如实报告「原配置未自动恢复」。
    if (err.find("无可用 .bak") == std::string::npos) return false;
    if (err.find("未自动恢复") == std::string::npos) return false;
    // 不写盘 ⇒ config.json 不存在（坏文件仅在隔离产物里）。
    if (std::filesystem::exists(cfgPath)) return false;
    if (listCorruptConfigs(dir).size() != 1) return false;

    // ③ 端到端后果：下次启动走「不存在」分支 ⇒ 不判 rejected（降级为默认值；密钥坏 ⇒ 有告警可见）。
    rk_win::WinJsonConfigStore store2;
    std::string warn2;
    store2.initialize(warn2);
    if (store2.configRejectedOnLoad()) return false;   // 非 rejected：config.json 本就不存在
    if (store2.initWarning().empty()) return false;    // 密钥仍坏 ⇒ degraded 必须在诊断上可见
    return true;
}

// ④（对抗审查收口）：initialize 成功分支里的级联路径迁移写盘失败**不再被吞** ⇒ 并入 warnOut/initWarning_
//   ⇒ health 体现为 degraded。此前 (void) 吞掉且成功分支无条件清空 initWarning_ ⇒ 迁移未持久化却报 ok。
// 构造（确定性）：迁移写盘走 writeAtomicallyWithBackup，其临时名为固定 `config.json.tmp`；用**目录**占住
//   该名 ⇒ writeFileAll 必失败 ⇒ 迁移写盘必失败（且不影响读取既有 config.json）。
bool test_config_cascade_migration_write_failure_degrades() {
    const auto dir = makeTempDir("cascade_migration_write_fail");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 1) 落盘默认配置，再把 cascadePath 经 PUT 指到旧的 tests/data 路径（resolvePathFromExeDir 转绝对路径）。
    rk_win::WinJsonConfigStore seed;
    std::string w;
    seed.initialize(w);
    if (!seed.updateFromJsonBody("{\"recognition\":{\"cascadePath\":\"tests/data/lbpcascade_frontalface.xml\"}}").ok) {
        return false;
    }
    // 读回确认「旧 cascade 路径」且该文件不存在（否则迁移分支不会触发 ⇒ 本用例构造失败，响亮返回）。
    std::filesystem::path oldCascadePath;
    {
        std::string txt;
        if (!readAllText(cfgPath, txt)) return false;
        rk_win::JsonValue doc;
        std::string perr;
        if (!rk_win::parseJson(txt, doc, perr) || !doc.isObject()) return false;
        const rk_win::JsonValue* rec = doc.find("recognition");
        if (!rec || !rec->isObject()) return false;
        const rk_win::JsonValue* cp = rec->find("cascadePath");
        if (!cp || !cp->isString()) return false;
        oldCascadePath = cp->s;
    }
    if (oldCascadePath.empty()) return false;
    if (std::filesystem::exists(oldCascadePath)) return false;  // 场景前提：旧文件确实不存在

    // 2) 用目录占住写盘临时名 config.json.tmp ⇒ 迁移写盘必失败。
    // ⚠️ **隐式耦合（钉住，勿无意打破）**：本构造依赖「initialize **不清理** config.json.tmp 这个名字」——
    //    若将来把 FACE-CONFIG-014 的启动清扫「对称化」地扩到 config.json.tmp（与本轮的 rollback.tmp 并列），
    //    该目录占位会在 initialize 开始时被删掉 ⇒ 迁移写盘**反而成功** ⇒ 本用例前提被**静默**打破（断言失真
    //    且不报错）。本批刻意**只清 rollback.tmp**：它由 reloadFromDisk 回滚的**固定名**产生、可被进程崩溃残留；
    //    而 config.json.tmp 由 writeAtomicallyWithBackup 每次写盘即时产生并即时消费，**不清它正是本用例的构造基础**。
    //    （判别补强见 test_config_initialize_cleans_stale_rollback_tmp 中「config.json.tmp 必须仍在」的断言。）
    {
        std::error_code ec;
        std::filesystem::create_directory(dir / "config.json.tmp", ec);
        if (ec) return false;
    }

    // 3) 重新 initialize（读既有 config.json）⇒ 命中迁移分支、写盘失败 ⇒ 必须降级可见。
    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    if (warn.find("写入 config.json 失败") == std::string::npos) return false;  // 区分「写盘失败」与「闸门触发」，失败不再被吞
    if (store.initWarning().empty()) return false;                             // health 可见（degraded）
    if (store.configRejectedOnLoad()) return false;                            // 成功分支：不是 rejected
    // 内存已改指新路径（迁移在内存生效），只是未落盘。
    if (store.current().recognition.cascadePath.string().find("assets") == std::string::npos) return false;
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

// FACE-CONFIG-012：装载失败时 initialize 的**独立可见通道**（stderr 直写，FACE-CONFIG-001）必须有输出，
// 且含失败原因与（隔离成功时的）隔离文件名。此前该通道**零测试覆盖**（stderr 直写无人断言）。
// 判别力：删掉 initialize 里那句 std::cerr 直写（或把它降级为仅写 EventLogger）⇒ captured 为空 ⇒ 红。
bool test_config_load_failure_writes_stderr() {
    const auto dir = makeTempDir("stderr_load_failure");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 坏 JSON（截断）：readFileAll 成功、parseJson 失败 ⇒ 判定为「无法装载」⇒ 隔离 + stderr 输出。
    const std::string bad = "{\"camera\": ";
    if (!writeAllText(cfgPath, bad)) return false;

    rk_win::WinJsonConfigStore store;
    std::string warn;
    std::string captured;
    {
        StderrCapture cap(dir / "stderr_capture.log");
        store.initialize(warn);
        captured = cap.finish();
    }
    // 场景已构造（否则后续断言无意义）。
    if (!store.configRejectedOnLoad()) return false;
    // 失败原因原文必须出现在 stderr 通道上。
    if (captured.find("配置装载失败") == std::string::npos) return false;
    if (captured.find("JSON 解析失败") == std::string::npos) return false;
    // 隔离文件名（隔离成功时）也必须出现在 stderr 通道上。
    const std::string qname = store.initQuarantineFileName();
    if (qname.empty()) return false;
    if (captured.find(qname) == std::string::npos) return false;
    return true;
}

// FACE-CONFIG-012（另一半，同样重要）：合法配置装载成功时 stderr **必须为空**——正常启动零输出，
// 不得污染既有输出（含 harness / ctest 的 stderr）。
bool test_config_healthy_load_silent_stderr() {
    const auto dir = makeTempDir("stderr_healthy_silent");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 先生成一份合法 config.json（首次 initialize 走默认值落盘；该次不在捕获区间内）。
    {
        rk_win::WinJsonConfigStore seed;
        std::string w;
        seed.initialize(w);
    }
    if (!std::filesystem::exists(cfgPath)) return false;

    rk_win::WinJsonConfigStore store;
    std::string warn;
    std::string captured;
    {
        StderrCapture cap(dir / "stderr_capture.log");
        store.initialize(warn);
        captured = cap.finish();
    }
    if (!warn.empty()) return false;               // 健康装载无告警
    if (store.configRejectedOnLoad()) return false;
    return captured.empty();                       // ★ 正常启动零输出
}

// FACE-CONFIG-014：initialize 必须清扫上一次进程崩溃残留的回滚临时文件 config.json.rollback.tmp。
//
// 背景：reloadFromDisk 的回滚写回用**固定名** config.json.rollback.tmp（writeFileAll → moveReplaceWriteThrough），
//   进程若在这两步之间崩溃会残留该文件；修复前 initialize 与 writeAtomicallyWithBackup 都不清理它。
// 判别力：
//   (1) 正对照：预置一份残留 config.json.rollback.tmp ⇒ initialize 后必须**不存在** ⇒ 红在「不清扫」上。
//   (2) 负对照：清理必须**只针对该固定名**、不得波及合法资产——预置 `.bak` 与一份隔离产物
//       `config.json.corrupt.<毫秒>`，initialize 后它们（以及 config.json 本身）必须仍在；若清理被写成
//       remove_all / 通配删除，或误删 config.json，本组断言立即变红。
bool test_config_initialize_cleans_stale_rollback_tmp() {
    const auto dir = makeTempDir("init_clean_rollback_tmp");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 1) 先落盘一份合法配置（负对照载体之一）。
    {
        rk_win::WinJsonConfigStore seed;
        std::string w;
        seed.initialize(w);
    }
    if (!std::filesystem::exists(cfgPath)) return false;

    // 2) 预置崩溃残留（固定名）+ 两类**不得**被波及的合法资产。
    const std::filesystem::path rbTmp = dir / "config.json.rollback.tmp";
    const std::filesystem::path bakFile = dir / "config.json.bak";
    const std::filesystem::path corruptSample = dir / "config.json.corrupt.1700000000000";
    // 3b) FACE-CONFIG-014 判别补强：再预置一个**普通文件** config.json.tmp ⇒ initialize 后它必须仍在。
    //   若清扫被写成「同时清 config.json.tmp」（或通配删除），下面 §4 的对应断言立即变红 ⇒ 使
    //   「清扫**只**针对 rollback.tmp」有可执行判据（否则实现即使误清该名，本组其余断言仍会照绿）。
    //   ⚠️ 这里刻意用**普通文件**而非目录：目录占位是 test_config_cascade_migration_write_failure_degrades
    //   的构造手段（用**另一临时目录**强制迁移写盘失败），两者目录不同、互不干扰，但语义不同——勿混淆。
    const std::filesystem::path cfgTmp = dir / "config.json.tmp";
    if (!writeAllText(rbTmp, "{\"schemaVersion\":1}")) return false;
    if (!writeAllText(bakFile, "{\"schemaVersion\":1}")) return false;
    if (!writeAllText(corruptSample, "evidence")) return false;
    if (!writeAllText(cfgTmp, "{\"schemaVersion\":1}")) return false;
    if (!std::filesystem::exists(rbTmp)) return false;  // 场景已构造（否则后续断言退化）

    // 3) initialize 必须清扫残留。
    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (std::filesystem::exists(rbTmp)) return false;   // ★ 残留未清 ⇒ 红
    // ★ 清扫只针对 rollback.tmp：config.json.tmp 不得被扫（否则变红）。
    if (!std::filesystem::exists(cfgTmp)) return false;

    // 4) 负对照：清理只针对该固定名，合法资产不受影响，且 config.json 仍可再次正常装载。
    if (!std::filesystem::exists(cfgPath)) return false;
    if (!std::filesystem::exists(bakFile)) return false;
    if (!std::filesystem::exists(corruptSample)) return false;
    if (!std::filesystem::exists(cfgTmp)) return false;
    rk_win::WinJsonConfigStore store2;
    std::string warn2;
    store2.initialize(warn2);
    if (!warn2.empty()) return false;
    if (store2.configRejectedOnLoad()) return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FACE-ARCWIN-010（S-6）：identifyThreshold 超过 LBPH 卡方距离的物理上界 64 时，
// **就地钳为 64 并告警**，绝不因「schema 上界从 100000 收紧到 64」把既有配置判为非法。
//
// 为什么这是本批最关键的一条：schema 收紧与钳制必须**同批**且钳制**在前**。若只有 schema 收紧而没有
// 钳制（或有人日后删掉钳制），任何既有 config.json 里 > 64 的值会在启动校验被拒 ⇒
// WinJsonConfigStore::initialize 走「隔离坏配置 + 以默认值落盘」⇒ 用户配置被弃用（用户明确列为红线）。
// 本用例即该红线的可执行判据：
//   · 删掉 normalizeRecognitionKeys 里的钳制（只留 schema 的 max=64）⇒ 启动被判 rejected ⇒ 下面
//     「非 rejected / 无隔离产物 / 阈值 = 64 / enrollSamples 仍为 37」四条同时变红；
//   · 把 schema 的 max 改回 100000 ⇒ 阈值会保持 99999（≠ 64）⇒ 值断言变红；
//   · 把钳制上界常数改成别的值 ⇒ 值断言（阈值 != 64）必红，但**红的先后取决于改大还是改小**：
//     改**大**（如 100）⇒ 钳后值落到运行时 schema 的 [1, 64] 之外 ⇒ 启动被判 rejected ⇒
//       「非 rejected / 无隔离产物」两条**先行**变红，值断言因 `return false` 短路**根本执行不到**；
//     改**小**（如 32）⇒ 钳后值仍在 [1, 64] 内、装载成功 ⇒ 直接停在值断言（读到 32 != 64）。
//     ⚠️ 不可笼统写成「与 64.5 断言同时变红」：上界改常数**不改变**运行时 schema 的 `maximum: 64`
//     （在 JsonSchemaValidator.cpp），且本用例前段一旦 `return false`，后段断言一律不执行。
bool test_config_lbph_threshold_over_physical_max_clamped() {
    // 纯函数边界（与三方 schema 的 1/64 同值）：clamp 输出**恒**落在 schema 合法区间内，
    // 这正是「钳完仍不合法 ⇒ 启动被拒」死角被消灭的判据。
    if (rk_win::clampIdentifyThreshold(-5.0) != 1.0) return false;
    if (rk_win::clampIdentifyThreshold(0.0) != 1.0) return false;
    if (rk_win::clampIdentifyThreshold(1.0) != 1.0) return false;
    if (rk_win::clampIdentifyThreshold(55.0) != 55.0) return false;  // 默认值必须原样通过（健康装载零告警）
    if (rk_win::clampIdentifyThreshold(64.0) != 64.0) return false;
    if (rk_win::clampIdentifyThreshold(99999.0) != 64.0) return false;

    const auto dir = makeTempDir("lbph_threshold_clamp");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    // 1) 落地一份默认（合法）配置作为「既有配置」基底。
    {
        rk_win::WinJsonConfigStore seed;
        std::string w;
        seed.initialize(w);
    }
    // 2) 改成「越界阈值 + 一个非默认可判别标记」，模拟既有用户的 config.json。
    std::string base;
    if (!readAllText(cfgPath, base)) return false;
    rk_win::JsonValue doc;
    std::string perr;
    if (!rk_win::parseJson(base, doc, perr) || !doc.isObject()) return false;
    rk_win::JsonValue* rec = doc.find("recognition");
    if (!rec || !rec->isObject()) return false;
    rec->o["identifyThreshold"] = rk_win::JsonValue::makeNumber(99999.0);
    rec->o["enrollSamples"] = rk_win::JsonValue::makeNumber(37);  // 「配置未被清空」的可判别标记
    if (!writeAllText(cfgPath, rk_win::toJsonString(doc, true))) return false;

    // 3) 启动装载：必须**成功**（非 rejected、无隔离产物），阈值被钳为 64，用户其余字段完好。
    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (store.configRejectedOnLoad()) return false;  // ★ 红线：绝不能被判为「无法装载」
    if (!store.initQuarantineFileName().empty()) return false;
    if (!listCorruptConfigs(dir).empty()) return false;  // 不得产生隔离证据（= 配置未被弃用）
    if (store.current().recognition.identifyThreshold != 64.0) return false;
    if (store.current().recognition.enrollSamples != 37) return false;  // 未被默认值静默替换
    // 告警必须可见——health 的 configWarning + configStatus=degraded 正是由 warnOut/initWarning_ 驱动。
    if (warn.empty()) return false;
    if (store.initWarning() != warn) return false;
    if (warn.find("identifyThreshold") == std::string::npos) return false;
    if (!std::filesystem::exists(cfgPath)) return false;  // initialize 成功分支不重写 config.json

    // 4) 三方一致性：运行时 schema 的该键上界确实已收到 64（config.schema.json / openapi.yaml 由
    //    tests/scripts/config-contract.test.js 对拍）。64.5 必须被拒、边界 64 必须被接受。
    {
        const std::string json = store.currentRedactedJsonPretty();
        rk_win::JsonValue d;
        std::string e;
        if (!rk_win::parseJson(json, d, e) || !d.isObject()) return false;
        rk_win::JsonValue* r2 = d.find("recognition");
        if (!r2 || !r2->isObject()) return false;
        r2->o["identifyThreshold"] = rk_win::JsonValue::makeNumber(64.5);
        std::string serr;
        if (rk_win::JsonSchemaValidator::validate(
                rk_win::JsonSchemaValidator::buildSettingsDocSchema(), d, &serr)) {
            return false;  // 64.5 被接受 ⇒ schema 上界未收收到 64
        }
        r2->o["identifyThreshold"] = rk_win::JsonValue::makeNumber(64.0);
        serr.clear();
        if (!rk_win::JsonSchemaValidator::validate(
                rk_win::JsonSchemaValidator::buildSettingsDocSchema(), d, &serr)) {
            return false;  // 边界值 64 必须合法（它正是钳制的落点）
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FACE-CONFIG-017（S-3）：log 段的 maxFileBytes × maxRollFiles 超总预算时**就地钳制并告警**。
//
// 口径（本批由「×2 份文件集」订正为「×3 份」，再由「×3」订正为「×4」）：落盘总量 =
// maxFileBytes × (maxRollFiles + 1)，而落盘面为**最多 4 份**文件集 —— recognition.csv 与
// recognition.jsonl（StructuredLogger）、**event_log.jsonl**（EventLogger **复用同一对上限、同一目录**，
// 三份由 win_local_service 写），以及 **render_metrics.jsonl**（RenderMetricsLogger，**仅旧版 UI 目标**
// win_camera_face_recognition 写）⇒ 实占磁盘为上式 ×4，且四份**合计**受 kLogBudgetBytes = 512 MiB
// 总预算约束（单份均分额 kLogPerSetBudgetBytes = kLogBudgetBytes / 4 = 134217728 B = 128 MiB）。
// ⚠️ 除数取 4 而非 3 的推导见 WinConfig.h 的 kLogFileSetCount：两个可执行体写入的份数不同
// （服务 3 份、旧版 UI 4 份），而 maxFileBytes / maxRollFiles 是**同一份 config.json 的两个键**、
// 被两者共用 ⇒ 预算要成立就必须按**写得多**的那个分配。
// schema 允许 1 GiB × 1001 ⇒ 乘以 4 份后最坏约 4 TiB ⇒ 可通过配置造成磁盘耗尽。
// 判别点：删掉 normalizeLogBudgetKeys（或钳制函数退化为恒等）⇒ store.current() 仍是 1 GiB / 1000
// ⇒ **值断言先行变红**（预算断言在其后，因 `return false` 短路而**不会被观测到**——两句是一起被该
// 变异违反的，但红灯只有一条）；把钳制写成「拒绝」⇒ rejected 断言先红（那正是要避免的红线形态）。
bool test_config_log_budget_clamped_to_total_budget() {
    constexpr std::uint64_t kTenMiB = 10ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t kOneGiB = 1024ULL * 1024ULL * 1024ULL;
    // 默认值必须**原样通过**（健康装载零告警的前提）。
    if (rk_win::clampLogMaxFileBytes(kTenMiB) != kTenMiB) return false;
    if (rk_win::clampLogMaxRollFiles(kTenMiB, 5) != 5) return false;
    // 越界值被压到**单份均分额**内：单文件上界 = 均分额 / 2；轮转档数按均分额反推
    // （1 GiB 时下界撞 schema 的最小值 1）。
    if (rk_win::clampLogMaxFileBytes(kOneGiB) != rk_win::kLogMaxFileBytesCap) return false;
    if (rk_win::clampLogMaxRollFiles(kOneGiB, 1000) != 1) return false;
    // 10 MiB：均分额 / 10 MiB = 134217728 / 10485760 = 12（无符号除法即下取整）⇒ 上界 = 12 − 1 = 11。
    if (rk_win::clampLogMaxRollFiles(kTenMiB, 1000) != 11) return false;
    // 只压上界、不抬下界（下界属既有 schema 契约，放宽会改变「既有非法配置被拒」的语义）：
    if (rk_win::clampLogMaxFileBytes(0) != 0) return false;
    if (rk_win::clampLogMaxRollFiles(kTenMiB, 0) != 0) return false;

    const auto dir = makeTempDir("log_budget_clamp");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    {
        rk_win::WinJsonConfigStore seed;
        std::string w;
        seed.initialize(w);
    }
    std::string base;
    if (!readAllText(cfgPath, base)) return false;
    rk_win::JsonValue doc;
    std::string perr;
    if (!rk_win::parseJson(base, doc, perr) || !doc.isObject()) return false;
    rk_win::JsonValue* lg = doc.find("log");
    if (!lg || !lg->isObject()) return false;
    // schema **恰好允许**的最大组合 ⇒ 只能靠运行时钳制收敛（schema 上下界按本批约束不动）。
    lg->o["maxFileBytes"] = rk_win::JsonValue::makeNumber(static_cast<double>(kOneGiB));
    lg->o["maxRollFiles"] = rk_win::JsonValue::makeNumber(1000);
    rk_win::JsonValue* rec = doc.find("recognition");
    if (!rec || !rec->isObject()) return false;
    rec->o["enrollSamples"] = rk_win::JsonValue::makeNumber(37);  // 「配置未被清空」的可判别标记
    if (!writeAllText(cfgPath, rk_win::toJsonString(doc, true))) return false;

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    if (store.configRejectedOnLoad()) return false;  // ★ 只钳不拒：绝不因预算超限判配置不可装载
    if (!listCorruptConfigs(dir).empty()) return false;
    const rk_win::AppConfig cfg = store.current();
    if (cfg.log.maxFileBytes != rk_win::kLogMaxFileBytesCap) return false;
    if (cfg.log.maxRollFiles != 1) return false;
    // ★ 单份均分额判据（口径与钳制函数一致：maxFileBytes × (maxRollFiles + 1) ≤ **单份均分额**）。
    //   注意**不要**与 kLogBudgetBytes 比较——那是四份**合计**的总预算，拿单份乘积去比虽然仍会通过，
    //   但会放过「单份超均分额、四份合计恰好未超」的实现漂移（口径失准）。
    if (cfg.log.maxFileBytes * (static_cast<std::uint64_t>(cfg.log.maxRollFiles) + 1) >
        rk_win::kLogPerSetBudgetBytes) {
        return false;
    }
    if (cfg.recognition.enrollSamples != 37) return false;  // 配置未被默认值替换
    if (warn.empty()) return false;
    if (warn.find("maxFileBytes") == std::string::npos) return false;
    if (warn.find("maxRollFiles") == std::string::npos) return false;
    if (!std::filesystem::exists(cfgPath)) return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FACE-CONFIG-018（S-1）之一：**写型**路径字段含 `..` 段时，PUT 必须真正**拒绝**（400）。
//
// 为什么只在 PUT 拒绝：启动期拒绝会触发 INI 回落清空用户配置（红线），故启动期只告警（见下一用例）。
// 为什么只查 patch 显式携带的字段：否则既有 config.json 里已存在 `..` 的用户会「任何 PUT 都被 400」，
// 被锁死在无法保存任何设置的状态。
// 为什么绝对路径**不**被拒：官方用法（`D:\ProgramData\NCNN\...`）且默认配置本身即绝对路径 ⇒
// 一刀切会误伤既有合法部署（见 checkConfigPath 的逃生口说明）。
// 判别点：删掉 updateFromJsonBody 里的写型检查块 ⇒ (1)(2) 两条的 res.ok 变为 true ⇒ 红；
//   把检查推广到读型字段 ⇒ (3) 的 cascadePath 用例变红；把绝对路径也判为违规 ⇒ (4) 变红。
bool test_config_write_path_parent_ref_put_rejected() {
    const auto dir = makeTempDir("write_path_put_reject");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    const rk_win::AppConfig before = store.current();
    const std::uint64_t gen0 = store.generation();

    // (1) 写型字段 log.logDir 含 `..` 段 ⇒ 400，且内存/磁盘配置**都**不受影响。
    const auto r1 = store.updateFromJsonBody("{\"log\":{\"logDir\":\"..\\\\escaped_logs\"}}");
    if (r1.ok) return false;
    if (r1.httpStatus != 400) return false;
    if (store.generation() != gen0) return false;                       // 未提交 ⇒ 代数不变
    if (store.current().log.logDir != before.log.logDir) return false;  // 配置未被改动（更未清空）
    if (r1.details.empty()) return false;                               // 失败原因须可见

    // (2) 写型字段 recognition.databasePath 含 `..` 段（正斜杠形态）⇒ 同样被拒。
    if (store.updateFromJsonBody("{\"recognition\":{\"databasePath\":\"a/../b/db.yml\"}}").ok) return false;

    // (3) 读型字段（cascadePath）本批**只告警、不拒绝**（分阶段强度）⇒ 必须 200。
    const auto r2 = store.updateFromJsonBody(
        "{\"recognition\":{\"cascadePath\":\"../assets/lbpcascade_frontalface.xml\"}}");
    if (!r2.ok) return false;

    // (4) 绝对路径是官方用法 ⇒ 写型字段上**不得**被拒。
    if (!store.updateFromJsonBody("{\"log\":{\"logDir\":\"D:\\\\ProgramData\\\\rk_wfcr_logs\"}}").ok) {
        return false;
    }
    return store.current().log.logDir == std::filesystem::path(L"D:\\ProgramData\\rk_wfcr_logs");
}

// ─────────────────────────────────────────────────────────────────────────────
// FACE-CONFIG-018（S-1）之二：**启动期**（initialize）遇到含 `..` 段的路径字段时**只告警**，
// 绝不判为「无法装载」——这是「不踩红线」在该项上的可执行判据。
//
// 判别点：若有人把 warnUnsafeConfigPaths 的输出改成 return false（或让路径检查参与 schema 拒绝）⇒
//   「非 rejected / 无隔离产物 / enrollSamples 仍为 37」三条同时变红；
//   若删掉路径检查 ⇒ warn 为空、且找不到 logDir / 写型 关键词 ⇒ 红。
bool test_config_write_path_startup_warns_not_rejected() {
    const auto dir = makeTempDir("write_path_startup_warn");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    {
        rk_win::WinJsonConfigStore seed;
        std::string w;
        seed.initialize(w);
    }
    std::string base;
    if (!readAllText(cfgPath, base)) return false;
    rk_win::JsonValue doc;
    std::string perr;
    if (!rk_win::parseJson(base, doc, perr) || !doc.isObject()) return false;
    rk_win::JsonValue* lg = doc.find("log");
    if (!lg || !lg->isObject()) return false;
    lg->o["logDir"] = rk_win::JsonValue::makeString("..\\escaped_logs");  // 写型字段 + 含 `..` 段
    rk_win::JsonValue* rec = doc.find("recognition");
    if (!rec || !rec->isObject()) return false;
    rec->o["enrollSamples"] = rk_win::JsonValue::makeNumber(37);  // 「配置未被清空」的可判别标记
    if (!writeAllText(cfgPath, rk_win::toJsonString(doc, true))) return false;

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    if (store.configRejectedOnLoad()) return false;  // ★ 红线：启动期路径违规只能是告警
    if (!store.initQuarantineFileName().empty()) return false;
    if (!listCorruptConfigs(dir).empty()) return false;
    if (warn.empty()) return false;
    if (warn.find("logDir") == std::string::npos) return false;
    if (warn.find("写型") == std::string::npos) return false;  // 写/读型的区分必须体现在告警里
    if (store.current().recognition.enrollSamples != 37) return false;
    if (!std::filesystem::exists(cfgPath)) return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ★ 2026-09-29 新增（项 2）：「热重载隔离竞态」的**确定性判别用例**（S4：该修复此前零判别锚 ——
//   改回锁外/去掉复核 ⇒ 全绿，因纯黑盒无法确定性观测交错）。
//
// 被钉的修复：把「复核 mtime 未变 + quarantineConfigFile」放进**同一个 mu_ 临界区**，使「读坏文件 →
//   并发 PUT 写盘合法 B 并提交 cfg_=B → watch 线程据此复核」的交错下**不再隔离刚写好的合法 B**。
// 判别构造（借**测试接缝** setReloadTestHookForTesting 制造交错）：接缝位于「解析被拒之后、重新取锁
//   复核 mtime 之前」，此刻**未持 mu_** ⇒ 钩子内可同步调 updateFromJsonBody（合法 PUT）。
//   · 修复后：复核 mtime 发现 PUT 已改文件 ⇒ **放弃隔离**、outErr 如实说明、无 corrupt 产物、磁盘=PUT 的配置。
//   · 变异①（**去掉 mtime 复核**）：隔离会把 PUT 写的合法 B 改名成 config.json.corrupt.* ⇒ 本例
//     (b)「无 corrupt 产物」与 (c)「outErr 不谎称隔离」两条变红。
//   · 变异②（**把复核/隔离移回锁外**，但**保留复核**）：**本例仍绿、不能判别** —— 本接缝是**同线程同步**的
//     （钩子在锁外同步调一次 PUT，PUT 在钩子返回前**已完成**）⇒ 无论复核在锁内还是锁外，单线程都读到新
//     mtime、都放弃隔离 ⇒ 无 corrupt、err 亦不谎称隔离。实测已确认：仅删掉复核段外的 `lock_guard<mu_>`
//     时本例仍 TEST_PASS。⇒ **本用例只判别「有没有复核」，不判别「复核/隔离的锁归属」**；后者由
//     test_config_reload_isolation_holds_mu_across_recheck_and_rename（异步接缝）判别。
bool test_config_reload_isolation_skips_when_put_races() {
    const auto dir = makeTempDir("reload_isolation_race");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    // 接缝：在「解析被拒之后、重新取锁复核之前」同步触发**一次**合法 PUT（改一个可判别字段）。
    //   ⚠️ 只触发一次（atomic 去重）：修复后的重试/复核路径可能多次经过此处，反复改文件会掩盖结果。
    std::atomic<bool> fired{false};
    store.setReloadTestHookForTesting([&store, &fired]() {
        if (fired.exchange(true)) return;
        (void)store.updateFromJsonBody("{\"camera\":{\"width\":777}}");  // 此刻未持 mu_ ⇒ 不自锁
    });

    // 外部把 config.json 改成**坏内容**（JSON 可解析、schema 拒绝：minFaceSizePx 期望 number）并拨快 mtime。
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
    // 使 wt（复核读到的坏文件 mtime）明显**晚于** PUT 的写入时刻，避开文件系统时间戳粒度盲区。
    if (!bumpMtimePlus2s(cfgPath)) return false;

    bool applied = true;
    std::string err;
    store.pollReloadOnce(applied, err);
    store.setReloadTestHookForTesting(nullptr);  // 幂等收尾，避免污染后续用例

    if (applied) return false;  // 坏内容不得被应用
    // (a) 磁盘上的配置必须是 PUT 写入的那份（camera.width == 777）。
    std::string back;
    if (!readAllText(cfgPath, back)) return false;
    rk_win::JsonValue backDoc;
    std::string berr;
    if (!rk_win::parseJson(back, backDoc, berr) || !backDoc.isObject()) return false;
    const rk_win::JsonValue* cam = backDoc.find("camera");
    if (!cam || !cam->isObject()) return false;
    const rk_win::JsonValue* wv = cam->find("width");
    if (!wv || !wv->isNumber() || wv->n != 777) {
        std::cerr << "[reload_isolation_race] 磁盘 camera.width 非 PUT 写入的 777"
                     "（隔离复核对并发 PUT 的处置偏离）" << std::endl;
        return false;
    }
    // (b) **没有**生成隔离产物（目录清单里不得出现 config.json.corrupt.*）。
    const auto corrupts = listCorruptConfigs(dir);
    if (!corrupts.empty()) {
        std::cerr << "[reload_isolation_race] 生成了 " << corrupts.size()
                  << " 个隔离产物（把刚 PUT 成功的合法配置当成坏文件改名了）" << std::endl;
        return false;
    }
    // (c) outErr **不谎称**隔离了坏配置。
    if (err.find("已隔离") != std::string::npos) {
        std::cerr << "[reload_isolation_race] outErr 谎称隔离: " << err << std::endl;
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ★ 2026-09-29 新增（项 3）：rotateKeyAndReencrypt 的「快照在锁外」竞态判别用例（A2 查出、本批修复）。
//
// 缺陷（修复前）：快照在锁外取、序列化也在锁外，只有「写盘 + 提交 cfg_」在 mu_ 内 ⇒ 并发 PUT 成功提交
//   的新配置会被 rotate 的**旧快照**静默覆盖（磁盘与内存一起退回旧值）。
// 修复：锁内记 generation_，写盘前在同一临界区复核未变；变了（期间有 PUT / 热重载提交）则恢复旧密钥、
//   重取快照重试（有界）；耗尽返回 409。
// 判别构造：测试接缝 setRotateTestHookForTesting 在「取快照之后、写盘之前」同步触发
//   一次合法 PUT（此刻未持 mu_；刻意在密钥替换之前，否则 PUT 会因 postUrl 密文与密钥错配而解析失败）。
//   断言 PUT 的配置在**内存与磁盘两侧**都存活。
//   · 修复后：rotate 复核 generation 发现变化 ⇒ 重取快照（含 PUT 的 width=999）⇒ 写盘 999、内存 999。
//   · 变异回旧序（快照在锁外且不复核 generation）：rotate 把旧快照 width=800 写回 ⇒ 磁盘/内存退 800 ⇒ 红。
bool test_config_rotate_does_not_clobber_concurrent_put() {
    const auto dir = makeTempDir("rotate_no_clobber_put");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (!store.updateFromJsonBody("{\"camera\":{\"width\":800}}").ok) return false;
    if (store.current().camera.width != 800) return false;

    // 接缝：只触发一次合法 PUT（width=999），制造「rotate 取快照 → 并发 PUT → rotate 写盘」交错。
    std::atomic<bool> fired{false};
    store.setRotateTestHookForTesting([&store, &fired]() {
        if (fired.exchange(true)) return;
        (void)store.updateFromJsonBody("{\"camera\":{\"width\":999}}");  // 此刻未持 mu_ ⇒ 不自锁
    });

    const auto res = store.rotateKeyAndReencrypt();
    store.setRotateTestHookForTesting(nullptr);

    if (!res.ok) {
        std::cerr << "[rotate_no_clobber_put] rotate 返回失败: code=" << res.code << " msg=" << res.message
                  << "（修复后应经代次复核重取快照后成功）" << std::endl;
        return false;
    }
    // (a) 内存配置 = PUT 的 width=999（旧序会退回 800）。
    if (store.current().camera.width != 999) {
        std::cerr << "[rotate_no_clobber_put] 内存 camera.width=" << store.current().camera.width
                  << "（期望 999）——rotate 的旧快照覆盖了已提交的 PUT" << std::endl;
        return false;
    }
    // (b) 磁盘配置 = width=999（与内存语义一致）。
    std::string back;
    if (!readAllText(cfgPath, back)) return false;
    rk_win::JsonValue backDoc;
    std::string berr;
    if (!rk_win::parseJson(back, backDoc, berr) || !backDoc.isObject()) return false;
    const rk_win::JsonValue* cam = backDoc.find("camera");
    if (!cam || !cam->isObject()) return false;
    const rk_win::JsonValue* wv = cam->find("width");
    if (!wv || !wv->isNumber() || wv->n != 999) {
        std::cerr << "[rotate_no_clobber_put] 磁盘 camera.width != 999 —— 磁盘被旧快照覆盖或与内存不一致"
                  << std::endl;
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ★ 2026-09-29 新增（项 A）：**密钥/密文同源不变量** —— rotate 与 PUT 交错后，盘上的 config.json 必须
//   能用盘上的密钥文件成功解密并解析（往返）。
//
// 被钉的不变量（承重，直接钉住而非实现细节）：**并发交错下**盘上的 config.json 恒能用盘上的密钥文件解开
//   （⚠️ **残余**：密钥文件与 config.json 是两个文件、无法原子同写 ⇒「写密钥之后、写 config.json 之前」
//   若**进程崩溃**仍会错配 —— 该崩溃窗口本批不消除，详见 WinJsonConfig.h 类级注释「项 A / 残余」）。
//   判据 = 新建一个**全新** WinJsonConfigStore 去装载盘上的文件——只有私钥能解开密文时才会成功，
//   否则 parseAndValidateSettingsDoc 解密失败 ⇒ 被隔离 + 降级启动（configRejectedOnLoad()==true）
//   ⇒ 断言 (b) 变红。故本用例**不**依赖任何实现细节（不提 generation_、不提锁）。
// 修复前的失效模式：rotate 的「新密钥文件写入」在 mu_ 之外，而 PUT 的加密（buildSettingsJson →
//   ensureOrLoadKey 每次重读密钥文件）也在 mu_ 之外 ⇒ 某 PUT 可在 rotate 写入新密钥**之后**读取密钥
//   并加密 config.json，随后 rotate 的代次复核发现变化 ⇒ 冲突分支 restoreOldKey() 把**旧**密钥写回
//   ⇒ 磁盘密钥=旧、磁盘密文=新 ⇒ 下次启动解密失败、用户设置静默丢失。
// 交错构造（可确定性构造的一种）：rotate 钩子在「取快照后、替换密钥之前」同步触发**一次**合法 PUT
//   （同时改 camera.width 与**敏感字段** poster.postUrl ⇒ 覆盖「重加密密文」路径）。修复后 rotate
//   经代次复核重取快照后成功；磁盘与内存都应留有 PUT 的值，且**新密钥能解开新密文**。
bool test_config_rotate_and_put_keep_disk_key_matched() {
    // 清空会改写配置解析结果的环境覆盖项（与既有用例一致，保证确定性）。
    SetEnvironmentVariableW(L"RK_WCFR_DNN_MODEL", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_DNN_CONFIG", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_HTTP_PORT", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_POST_URL", nullptr);

    const auto dir = makeTempDir("rotate_put_invariant");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (!store.updateFromJsonBody("{\"camera\":{\"width\":800}}").ok) return false;

    const std::string putUrl = "https://race.example/api/faces";
    std::atomic<bool> fired{false};
    store.setRotateTestHookForTesting([&store, &fired, &putUrl]() {
        if (fired.exchange(true)) return;
        (void)store.updateFromJsonBody(
            "{\"camera\":{\"width\":997},\"poster\":{\"postUrl\":\"" + putUrl + "\"}}");
    });

    const auto res = store.rotateKeyAndReencrypt();
    store.setRotateTestHookForTesting(nullptr);
    if (!res.ok) {
        std::cerr << "[rotate_put_invariant] rotate 返回失败: code=" << res.code << std::endl;
        return false;
    }

    // (a) 内存：PUT 的值存活（旧快照被代次复核拦下）。
    if (store.current().camera.width != 997) return false;
    if (store.current().poster.postUrl != putUrl) return false;

    // (b) ★ 不变量：全新 store 必须能装载盘上的 config.json（唯一判据 = 私钥能解开密文）。
    {
        rk_win::WinJsonConfigStore reloaded;
        std::string warn2;
        reloaded.initialize(warn2);
        if (reloaded.configRejectedOnLoad()) {
            std::cerr << "[rotate_put_invariant] 新密钥解不开盘上密文（不变量破坏）：" << warn2 << std::endl;
            return false;
        }
        if (!reloaded.initQuarantineFileName().empty()) return false;
        if (reloaded.current().camera.width != 997) return false;
        if (reloaded.current().poster.postUrl != putUrl) return false;
    }
    if (!listCorruptConfigs(dir).empty()) return false;
    return true;
}

// ★ 2026-09-29 新增（项 A）：**409 重试耗尽路径** + 不变量（上一轮明确登记为**零覆盖**）。
//
// 构造：rotate 钩子**每次 attempt 都**触发一次合法 PUT（不设去重 ⇒ 反复改 generation）⇒ rotate 每轮
//   代次复核都发现变化 ⇒ kMaxRotateAttempts 耗尽 ⇒ 返回**可见失败码**（409 / code=conflict）。
// 判别点：
//   (a) 返回码可见（此路径此前无任何运行时判据）；
//   (b) 事后的**不变量仍成立** —— 全新 store 必须能装载盘上的 config.json。修复后冲突分支**不写盘**
//       ⇒ 密钥/密文同源保持；若冲突分支改成「写新密钥（而不写 config.json）」或去掉「冲突不写盘」的
//       纪律 ⇒ 磁盘密钥=新、密文=旧 ⇒ 全新 store 解密失败 ⇒ (b) 红。
bool test_config_rotate_conflict_exhaustion_keeps_invariant() {
    SetEnvironmentVariableW(L"RK_WCFR_DNN_MODEL", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_DNN_CONFIG", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_HTTP_PORT", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_POST_URL", nullptr);

    const auto dir = makeTempDir("rotate_conflict_exhaust");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (!store.updateFromJsonBody("{\"camera\":{\"width\":800}}").ok) return false;

    // 每次 attempt 都触发一次 PUT（**无**去重）：rotate 每轮都会看到 generation 变化 ⇒ 有界重试耗尽。
    std::atomic<int> putCount{0};
    store.setRotateTestHookForTesting([&store, &putCount]() {
        const int n = putCount.fetch_add(1) + 1;
        (void)store.updateFromJsonBody("{\"camera\":{\"width\":" + std::to_string(800 + n) + "}}");
    });

    const auto res = store.rotateKeyAndReencrypt();
    store.setRotateTestHookForTesting(nullptr);

    // (a) 可见失败码：耗尽 ⇒ 409 / conflict（而非静默成功或旧快照覆盖）。
    if (res.ok) {
        std::cerr << "[rotate_conflict_exhaust] 期望失败但 rotate 返回成功" << std::endl;
        return false;
    }
    if (res.httpStatus != 409) {
        std::cerr << "[rotate_conflict_exhaust] 期望 409 但得到 " << res.httpStatus << std::endl;
        return false;
    }
    if (res.code != "conflict") return false;

    // (b) ★ 不变量：全新 store 必须能装载盘上的 config.json（密钥与密文同源）。
    {
        rk_win::WinJsonConfigStore reloaded;
        std::string warn2;
        reloaded.initialize(warn2);
        if (reloaded.configRejectedOnLoad()) {
            std::cerr << "[rotate_conflict_exhaust] 耗尽后盘上密文无法用盘上密钥解开：" << warn2 << std::endl;
            return false;
        }
        if (!reloaded.initQuarantineFileName().empty()) return false;
        if (!listCorruptConfigs(dir).empty()) return false;
    }
    return true;
}

// ★ 2026-09-29 新增（项 A）：普通 rotate（无并发）后敏感字段往返 —— 新密钥必须能解开 rotate 写下的密文。
// 直接钉「rotate 结束后磁盘密钥/密文同源」：PUT 一个真实 postUrl → rotate（换新密钥、重加密落盘）→
// 全新 store 装载 → postUrl 必须仍是真实值（若密钥与密文错配，装载会解密失败 ⇒ 走隔离+默认值 ⇒ 红）。
bool test_config_rotate_preserves_sensitive_field_roundtrip() {
    SetEnvironmentVariableW(L"RK_WCFR_DNN_MODEL", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_DNN_CONFIG", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_HTTP_PORT", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_POST_URL", nullptr);

    const auto dir = makeTempDir("rotate_sensitive_roundtrip");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    const std::string realUrl = "https://rotate.example/api/faces";
    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    if (!store.updateFromJsonBody("{\"poster\":{\"postUrl\":\"" + realUrl + "\"}}").ok) return false;
    if (store.current().poster.postUrl != realUrl) return false;

    const auto res = store.rotateKeyAndReencrypt();
    if (!res.ok) return false;

    rk_win::WinJsonConfigStore reloaded;
    std::string warn2;
    reloaded.initialize(warn2);
    if (reloaded.configRejectedOnLoad()) {
        std::cerr << "[rotate_sensitive_roundtrip] rotate 后盘上密文无法解开：" << warn2 << std::endl;
        return false;
    }
    if (!reloaded.initQuarantineFileName().empty()) return false;
    return reloaded.current().poster.postUrl == realUrl;
}

// ─────────────────────────────────────────────────────────────────────────────
// ★ 2026-09-29 新增（项 1）：给「复核 mtime 未变」与 `quarantineConfigFile` **同处一个 mu_ 临界区**这一
//   性质建立**判别锚**。
//
// 缺口（本批自陈）：既有 test_config_reload_isolation_skips_when_put_races 的接缝是同线程**同步**的——
//   钩子在**锁外**同步调一次 PUT，PUT 在钩子返回前就**已完成** ⇒ 无论复核在锁内还是锁外，单线程都读到新
//   mtime ⇒ 用例都绿。故那条用例只能判别「**有没有**复核」，**不能**判别「复核与 rename 是否同处一个临界区」。
//
// 本用例的构造：接缝 setReloadIsolationTestHookForTesting 位于「**复核 mtime 未变之后、rename 之前**」⇒
//   **此刻已持 mu_**。钩子只做**不加锁**的动作：唤起一个后台线程 P 去**尝试** PUT（camera.width=777），再
//   **有界等待** P 完成（原子标志 putDone；steady_clock 截止点 + 1ms 轮询）。关键判别：
//     · 正确实现（复核与 rename 同在 mu_ 内）：钩子持 mu_ ⇒ P 的 PUT 被 mu_ 挡在临界区外 ⇒ 持锁窗口内
//       putDone **恒假** ⇒ 断言 (a) 绿；复核通过后 rename 掉的是**原坏文件**（逐字节保全）⇒ 断言 (c) 绿。
//     · 变异（hook 点被移到 mu_ 之外）：P 立刻跑完 PUT（putDone 变真）⇒ 断言 (a) 红。
//   ★ 前置锚（本轮新增，**防判据空转**）：后台线程**在调用 PUT 之前**先置 putAttempting；钩子先**限时等待**
//      该标志为真，**观察不到即落 sceneBuilt=false**；主线程据 sceneBuilt **响亮 return false**
//      （文案「场景未能构造：后台线程未在限时内开始尝试 PUT」），**绝不允许静默通过**。否则若后台线程根本
//      没被调度起来，「putDone 仍为假」会**平凡成立** ⇒ 高负载下坏实现可能误绿。
//   ⚠️ 钩子在**持 mu_** 时被调用 ⇒ 钩子内**绝不可**调用任何会取 mu_ 的公共 API（mu_ 非递归 ⇒ 自锁死，比
//      断言失败更糟）——故钩子只置位 / 起线程 / 有界轮询原子量，**不碰** store 的任何公共方法。
//   ⚠️ **判别力边界（本轮由变异实测**收窄到恰好等于实测面**，绝不外推）**：断言 (a) 的判据就是
//      「**hook 点**是否持 mu_」。它在本轮实现下**恰好**钉住「hook 点持 mu_」这一条，而**不**钉住
//      「复核整体在 mu_ 内」或「rename 整体在 mu_ 内」——后两者是**更宽**的形态，本轮**实测**为**阴性/绿**：
//        (i) 把**复核**单独移出锁、而 hook 与 rename 仍留在锁内（hook 仍持锁 ⇒ (a) 绿）；
//        (ii) 复核与 rename 分属**两把不同的锁**（hook 落在其中一把内 ⇒ (a) 绿）。
//      ⇒ 本锚**只**能判别「hook 点持 mu_（即复核+rename 与 hook 同锁）」与「三者整体在锁外」两态；
//        **不能**判别复核/rename 各自的锁归属（拆分或单侧移出）。若将来要封死 (i)(ii)，须另设**复核之前**
//        的接缝与判据——勿据本用例误以为已覆盖。
//   ⚠️ watchdog（有界失败，不挂死 ctest）：钩子的等待有上限；对 P 的 join 亦有界——二者任一超时都**响亮
//      return false**，绝不无限等待。
//   ⚠️ 单次运行代价（**本轮实测订正**）：正确实现下 P 被锁挡住、永不完成 ⇒ Phase 2 必等满 kHookWait
//      （500ms）；Phase 1 另用**独立**的较短窗口 kAttemptWait（300ms），不占用 Phase 2 的预算 ⇒ 改用
//      steady_clock 截止点后**名义时长 == 实耗时**，本用例实测（5 次）≈ 0.55–0.57s。（此前按「迭代次数」
//      而非毫秒计时：500 次 sleep 的实际耗时由 Windows 默认定时器粒度与负载决定 —— 本机探针 500×
//      sleep(1ms) 轻载实测 4.0–7.2s，重载下 5 次采样的旧实耗 ≈ 7.8s，约名义值的 15.6×。）
//   📌 总量落点（本批，可复算）：本用例与 test_config_put_decrypt_shares_key_with_commit 的单次耗时已由
//      各 ≈7.8s 降到各 ≈0.55–0.60s ⇒ 两条合计省 ≈14.5s，与整二进制一次运行耗时由 ≈35s 降到约 20s
//      （本机实测 18.0–23.2s）同量级。
bool test_config_reload_isolation_holds_mu_across_recheck_and_rename() {
    const auto dir = makeTempDir("reload_isolation_mu");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);

    std::atomic<bool> putAttempting{false};  // ★ 前置锚：后台线程开始尝试 PUT 前先置位
    std::atomic<bool> putDone{false};
    std::atomic<bool> putWasBlockedInHook{false};
    std::atomic<bool> hookRan{false};
    std::atomic<bool> sceneBuilt{false};  // 钩子在限时内观察到「后台已开始尝试」
    std::thread putThread;

    // 时间基（steady_clock）：钩子窗口与 join 等待都以**真实截止点**判定，名义时长 == 实耗时。
    constexpr auto kAttemptWait = std::chrono::milliseconds(300);  // Phase 1：等「后台已开始尝试」（较短）
    constexpr auto kHookWait = std::chrono::milliseconds(500);     // Phase 2：等「是否被挡」（完整）
    constexpr auto kJoinWait = std::chrono::milliseconds(5000);

    // 钩子：**持 mu_** 时被调用。只起线程 + 有界等待，**不调** store 的公共 API（会自锁死）。
    store.setReloadIsolationTestHookForTesting([&]() {
        if (hookRan.exchange(true)) return;  // 只跑一次（对 joinable thread 二次赋值会 terminate）
        putThread = std::thread([&store, &putAttempting, &putDone]() {
            putAttempting.store(true);  // ★ 先置位，再真正调 PUT
            (void)store.updateFromJsonBody("{\"camera\":{\"width\":777}}");
            putDone.store(true);
        });
        // Phase 1（**较短**窗口）：等后台线程**确实开始尝试**（否则「未完成」会平凡成立）。
        const auto attemptDeadline = std::chrono::steady_clock::now() + kAttemptWait;
        while (!putAttempting.load() && std::chrono::steady_clock::now() < attemptDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!putAttempting.load()) {
            sceneBuilt.store(false);
            return;  // 场景未能构造 ⇒ 交主线程**响亮**失败（绝不静默通过）
        }
        sceneBuilt.store(true);
        // Phase 2（**独立**的完整窗口）：再测量「是否被挡」——从此刻**重新**计时，绝不复用 Phase 1
        //   的截止点。否则 Phase 1 的调度延迟会吃掉本段预算、使观测窗塌缩 ⇒ 坏实现（未持锁）可能
        //   因「窗口内没等到它完成」被判为「被锁挡住」⇒ 假绿。
        const auto observeDeadline = std::chrono::steady_clock::now() + kHookWait;
        while (!putDone.load() && std::chrono::steady_clock::now() < observeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        putWasBlockedInHook.store(!putDone.load());
    });

    // 外部把 config.json 改成坏内容（JSON 可解析、schema 拒绝：minFaceSizePx 期望 number）并拨快 mtime。
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

    bool applied = true;
    std::string err;
    store.pollReloadOnce(applied, err);
    store.setReloadIsolationTestHookForTesting(nullptr);  // 幂等收尾，避免污染后续用例

    // 有界等待 P 完成（正确实现下 P 在 reload 释放 mu_ 后才提交）。超时 ⇒ watchdog 有界失败。
    {
        const auto deadline = std::chrono::steady_clock::now() + kJoinWait;
        while (!putDone.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (putThread.joinable()) putThread.join();
    // ★ 前置锚先于判别断言：场景未构造 ⇒ 响亮失败（而非让「未完成」平凡成立）。
    if (!sceneBuilt.load()) {
        std::cerr << "[reload_isolation_mu] 场景未能构造：后台线程未在限时内开始尝试 PUT"
                     "（判据不成立，本用例失败而非静默通过）" << std::endl;
        return false;
    }
    if (!putDone.load()) {
        std::cerr << "[reload_isolation_mu] watchdog：后台 PUT 未在时限内完成（有界失败，不挂死）" << std::endl;
        return false;
    }

    // 坏内容不得被应用。
    if (applied) return false;
    // (a) ★ 判别锚一：复核+rename 持 mu_ 期间，PUT 必被挡在临界区外（未提交）。
    if (!putWasBlockedInHook.load()) {
        std::cerr << "[reload_isolation_mu] 复核持锁窗口内后台 PUT 已完成 ⇒ 复核/rename 未与 PUT 互斥"
                     "（复核或 rename 在 mu_ 之外）" << std::endl;
        return false;
    }
    // (b) 磁盘最终是 PUT 写入的那份（camera.width == 777）：确认交错确实发生、且最终状态自洽。
    std::string back;
    if (!readAllText(cfgPath, back)) return false;
    rk_win::JsonValue backDoc;
    std::string berr;
    if (!rk_win::parseJson(back, backDoc, berr) || !backDoc.isObject()) return false;
    const rk_win::JsonValue* cam = backDoc.find("camera");
    if (!cam || !cam->isObject()) return false;
    const rk_win::JsonValue* wv = cam->find("width");
    if (!wv || !wv->isNumber() || wv->n != 777) {
        std::cerr << "[reload_isolation_mu] 最终磁盘 camera.width 非 777（PUT 未落盘或未在 reload 后生效）"
                  << std::endl;
        return false;
    }
    // (c) ★ 判别锚二：隔离产物必须逐字节等于**原坏内容**（复核通过后才 rename ⇒ rename 掉的就是那份坏文件；
    //     若复核与 rename 分处不同临界区，rename 掉的会是 PUT 刚写的合法配置 ⇒ 内容不符 ⇒ 红）。
    const auto corrupts = listCorruptConfigs(dir);
    if (corrupts.size() != 1) {
        std::cerr << "[reload_isolation_mu] 隔离产物数 = " << corrupts.size() << "（期望 1）" << std::endl;
        return false;
    }
    std::string qtext;
    if (!readAllText(corrupts[0], qtext)) return false;
    if (qtext != bad) {
        std::cerr << "[reload_isolation_mu] 隔离产物内容 ≠ 原坏内容 ⇒ rename 掉的可能是并发 PUT 刚写的合法配置"
                  << std::endl;
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ★ 2026-09-29 新增（项 2）：给「PUT 的**解密**与提交同处一个 mu_ 临界区、以同一密钥」建立判别锚。
//
// 缺口（修复前）：updateFromJsonBody 的加密 + 写盘 + 提交已进 mu_，但**解密**
//   （parseAndValidateSettingsDoc → ensureOrLoadKey **每次重读密钥文件**）仍在锁外 ⇒ 存在交错：父代基底
//   base 在锁外读、其后 rotateKeyAndReencrypt 把密钥从 K1 换成 K2 并重写盘 ⇒ 解密 base 的 postUrl 密文
//   （K1 加密）时读到的却是 K2 ⇒ 解密失败 ⇒ 整次 PUT **响亮失败**（非静默、不损坏磁盘，但拒绝服务）。
//
// 本用例构造：接缝 setPutPreDecryptTestHookForTesting 位于「读 base / 合并 / schema 校验之后，解密之前」⇒
//   **此刻已持 mu_**（项 2 修复后）。钩子只做**不加锁**的动作：唤起后台线程 R 去**尝试** rotate，再有界等待
//   （steady_clock 截止点 + 1ms 轮询）。
//     · 正确实现（解密与提交同在 mu_ 内）：钩子持 mu_ ⇒ R 的 rotate 被挡在临界区外 ⇒ 解密仍用 K1
//       （= base 密文的密钥）⇒ PUT 成功 ⇒ 断言 (a)(b)(c) 绿。
//     · 变异（把 mu_ 缩回「仅加密 + 写盘 + 提交」、解密回到锁外，hook 点随之落在锁外）：R 先换密钥 K2 并
//       重写盘，PUT 再用 K2 解 base 的 K1 密文 ⇒ 失败 ⇒ 断言 (a) 红（(b) 亦红：R 在钩子窗口内已完成）。
//   ★ 前置锚（本轮新增，**防判据空转**）：后台线程**在调用 rotate 之前**先置 rotAttempting；钩子先**限时等待**
//      该标志为真，**观察不到即落 sceneBuilt=false**；主线程据 sceneBuilt **响亮 return false**
//      （文案「场景未能构造：后台线程未在限时内开始尝试 rotate」），**绝不允许静默通过**。否则若后台线程根本
//      没被调度起来，「rotDone 仍为假」会**平凡成立** ⇒ 高负载下坏实现可能误绿。
//   ⚠️ 钩子在**持 mu_** 时被调用 ⇒ 钩子内**绝不可**调用会取 mu_ 的公共 API（自锁死）——只置位 / 起线程 / 轮询。
//   ⚠️ **判别力边界（本轮由变异实测**收窄到恰好等于实测面**）**：断言 (b) 的判据就是「**hook 点**是否持
//      mu_」，故本锚**只**钉住「**解密**与提交同临界区」这一条。它**不**判别「**读父代基底
//      lastGoodJsonPretty_ **是否在锁内」——把**基底读取**单独移出锁、而解密仍在锁内时，hook 点仍在锁内 ⇒
//      R 仍被挡 ⇒ 断言 (a)(b)(c) 全绿（本轮**实测**为**阴性/绿**）。⇒ 切勿据本用例声称已封死
//      「基底锁外读取」这一交错；那需要**更靠前**（基底读取之前）的接缝，本轮未做。
//   ⚠️ watchdog（有界失败，不挂死 ctest）：钩子等待有上限、对 R 的 join 亦有界——任一超时都**响亮 return false**。
//   ⚠️ 单次运行代价（**本轮实测订正**）：正确实现下 R 被 mu_ 挡住、永不完成 ⇒ Phase 2 必等满 kHookWait
//      （500ms）；Phase 1 另用**独立**的较短窗口 kAttemptWait（300ms），不占用 Phase 2 的预算 ⇒ 改用
//      steady_clock 截止点后**名义时长 == 实耗时**，本用例实测（5 次）≈ 0.57–0.60s。（此前按「迭代次数」
//      而非毫秒计时：500 次 sleep 的实际耗时由 Windows 默认定时器粒度与负载决定，旧实耗 ≈ 7.8s，约名义
//      值的 15.6×。）
bool test_config_put_decrypt_shares_key_with_commit() {
    SetEnvironmentVariableW(L"RK_WCFR_DNN_MODEL", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_DNN_CONFIG", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_HTTP_PORT", nullptr);
    SetEnvironmentVariableW(L"RK_WCFR_POST_URL", nullptr);

    const auto dir = makeTempDir("put_decrypt_key");
    const auto cfgPath = dir / "config.json";
    ConfigEnvGuard env(cfgPath);

    rk_win::WinJsonConfigStore store;
    std::string warn;
    store.initialize(warn);
    // 先 PUT 一个真实 postUrl，使 base 含**以当前密钥加密**的密文（解密路径可达，非空明文退化路径）。
    const std::string url = "https://keyatomic.example/api/faces";
    if (!store.updateFromJsonBody("{\"poster\":{\"postUrl\":\"" + url + "\"}}").ok) return false;
    if (store.current().poster.postUrl != url) return false;

    std::atomic<bool> rotAttempting{false};  // ★ 前置锚：后台线程开始尝试 rotate 前先置位
    std::atomic<bool> rotDone{false};
    std::atomic<bool> rotBlockedInHook{false};
    std::atomic<bool> hookRan{false};
    std::atomic<bool> sceneBuilt{false};  // 钩子在限时内观察到「后台已开始尝试」
    std::thread rotThread;

    // 时间基（steady_clock）：钩子窗口与 join 等待都以**真实截止点**判定，名义时长 == 实耗时。
    constexpr auto kAttemptWait = std::chrono::milliseconds(300);  // Phase 1：等「后台已开始尝试」（较短）
    constexpr auto kHookWait = std::chrono::milliseconds(500);     // Phase 2：等「是否被挡」（完整）
    constexpr auto kJoinWait = std::chrono::milliseconds(5000);

    // 钩子：**持 mu_** 时被调用。只起线程 + 有界等待，**不调** store 的公共 API（会自锁死）。
    store.setPutPreDecryptTestHookForTesting([&]() {
        if (hookRan.exchange(true)) return;  // 只跑一次（对 joinable thread 二次赋值会 terminate）
        rotThread = std::thread([&store, &rotAttempting, &rotDone]() {
            rotAttempting.store(true);  // ★ 先置位，再真正调 rotate
            (void)store.rotateKeyAndReencrypt();
            rotDone.store(true);
        });
        // Phase 1（**较短**窗口）：等后台线程**确实开始尝试**（否则「未完成」会平凡成立）。
        const auto attemptDeadline = std::chrono::steady_clock::now() + kAttemptWait;
        while (!rotAttempting.load() && std::chrono::steady_clock::now() < attemptDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!rotAttempting.load()) {
            sceneBuilt.store(false);
            return;  // 场景未能构造 ⇒ 交主线程**响亮**失败（绝不静默通过）
        }
        sceneBuilt.store(true);
        // Phase 2（**独立**的完整窗口）：再测量「是否被挡」——从此刻**重新**计时，绝不复用 Phase 1
        //   的截止点（否则 Phase 1 延迟会吃掉本段预算 ⇒ 观测窗塌缩 ⇒ 坏实现可能被误判为「被挡」）。
        const auto observeDeadline = std::chrono::steady_clock::now() + kHookWait;
        while (!rotDone.load() && std::chrono::steady_clock::now() < observeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        rotBlockedInHook.store(!rotDone.load());
    });

    const auto res = store.updateFromJsonBody("{\"camera\":{\"width\":777}}");
    store.setPutPreDecryptTestHookForTesting(nullptr);  // 幂等收尾

    // 有界等待 R（正确实现下 R 在 PUT 释放 mu_ 后才轮换）。超时 ⇒ watchdog 有界失败。
    {
        const auto deadline = std::chrono::steady_clock::now() + kJoinWait;
        while (!rotDone.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (rotThread.joinable()) rotThread.join();

    // ★ 前置锚先于判别断言：场景未构造 ⇒ 响亮失败（而非让「未完成」平凡成立）。
    if (!sceneBuilt.load()) {
        std::cerr << "[put_decrypt_key] 场景未能构造：后台线程未在限时内开始尝试 rotate"
                     "（判据不成立，本用例失败而非静默通过）" << std::endl;
        return false;
    }
    if (!rotDone.load()) {
        std::cerr << "[put_decrypt_key] watchdog：后台 rotate 未在时限内完成（有界失败，不挂死）" << std::endl;
        return false;
    }

    // (a) ★ 判别锚：PUT 必须成功（解密读到的密钥 == base 密文所用的密钥）。
    if (!res.ok) {
        std::cerr << "[put_decrypt_key] PUT 失败（code=" << res.code << "）⇒ 解密读到的密钥 ≠ 基线密文所用的密钥"
                     "（解密落在 mu_ 之外、被并发 rotate 换了密钥）" << std::endl;
        return false;
    }
    // (b) ★ 判别锚：钩子持锁窗口内 rotate 必被挡在临界区外（未完成）。
    if (!rotBlockedInHook.load()) {
        std::cerr << "[put_decrypt_key] 钩子持锁窗口内后台 rotate 已完成 ⇒ 解密/提交未与 rotate 互斥"
                  << std::endl;
        return false;
    }
    // (c) PUT 的 width=777 必须落盘（且随后 rotate 以新密钥重加密 cfg_ 时保留它）。
    if (store.current().camera.width != 777) return false;
    std::string back;
    if (!readAllText(cfgPath, back)) return false;
    rk_win::JsonValue bd;
    std::string berr;
    if (!rk_win::parseJson(back, bd, berr) || !bd.isObject()) return false;
    const rk_win::JsonValue* cam = bd.find("camera");
    if (!cam || !cam->isObject()) return false;
    const rk_win::JsonValue* wv = cam->find("width");
    if (!wv || !wv->isNumber() || wv->n != 777) {
        std::cerr << "[put_decrypt_key] 磁盘 camera.width 非 777（PUT 未落盘，或未在随后 rotate 中保留）"
                  << std::endl;
        return false;
    }
    // (d) 不变量：全新 store 必须能装载盘上的 config.json（rotate 在 PUT 之后以新密钥重加密，仍同源）。
    {
        rk_win::WinJsonConfigStore reloaded;
        std::string w2;
        reloaded.initialize(w2);
        if (reloaded.configRejectedOnLoad()) {
            std::cerr << "[put_decrypt_key] 全新 store 无法装载（密钥/密文错配）：" << w2 << std::endl;
            return false;
        }
        if (reloaded.current().camera.width != 777) return false;
        if (reloaded.current().poster.postUrl != url) return false;
    }
    if (!listCorruptConfigs(dir).empty()) return false;
    return true;
}


