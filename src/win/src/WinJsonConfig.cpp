#include "rk_win/WinJsonConfig.h"

#include "AccelerationContract.h"
#include "rk_win/JsonLite.h"
#include "rk_win/WinCrypto.h"
#include "rk_win/JsonSchemaValidator.h"

#ifdef _WIN32
#include <windows.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>

namespace rk_win {
namespace {

[[maybe_unused]] constexpr const wchar_t* kProductNameW = L"rk_wcfr"; // Reserved for future visible product/app name
constexpr const wchar_t* kConfigDirNameW = L"rk_wcfr";                // Internal identifier for local filesystem paths
constexpr int kInferenceIntervalDefaultMs = 150;
constexpr int kInferenceIntervalMinMs = 80;
constexpr int kInferenceIntervalMaxMs = 500;

// 脱敏输出占位哨兵：toSettingsDocObject 对敏感字段（poster.postUrl）输出该字面量掩码。
// 回写语义 = "不修改该字段"（见 stripMaskedSentinels），防止全量回写销毁真实密文。
constexpr const char* kMaskedSentinel = "***";

constexpr std::uintmax_t kMaxConfigFileBytes = 10 * 1024 * 1024;      // 10MB max config file

// ── FACE-ARCWIN-010 / FACE-CONFIG-017 / FACE-CONFIG-018 的规范化告警回传通道 ──
// 为什么用 thread_local 而不是出参：本批允许改动的文件**不含** src/win/include/rk_win/WinJsonConfig.h，
//   而 parseAndValidateSettingsDoc 的签名（jsonText, cfgOut, outErr）正是在那里声明的 ⇒ 无法新增出参，
//   也不能把告警塞进 outErr（它是**失败**原因，调用方按「非空且返回 false」解读；成功路径上非空 outErr
//   会与 `if (!parseAndValidateSettingsDoc(...))` 的判读习惯冲突）。
//   三个调用点（initialize / updateFromJsonBody / reloadFromDisk，另有 reloadFromDisk 的 .bak 复验）
//   **全部**位于本文件，且都是「同一线程内调用 → 立即读取」⇒ 线程局部变量既无跨线程竞争，
//   也不会跨调用串味；每次 parseAndValidateSettingsDoc 入口清空，语义等价于出参。
// 通道去向上限（如实登记）：只有 initialize 把它并入 warnOut（⇒ health 的 configWarning +
//   configStatus=degraded，即用户可见的那条通道；该告警**跨启动持续**，见合并处的登记）。PUT 路径
//   无可用的对外通道——HTTP 层只在**错误**响应里带 details（JsonEndpointHandlers 的 errJson），改它
//   超出本批允许改动的文件范围 ⇒ PUT 的规范化告警在对外响应里不可见。
//   「钳制结果本身可见」只是**部分**成立，分字段说明见 updateFromJsonBody 末尾的 takeNormalizeWarnings
//   处（log 段钳后值经 GET 可见；identifyThreshold 的 `> 64` 在 PUT 上是 schema **400 拒绝**，
//   钳制分支不可达）。原注释在此一概而论，已订正。
thread_local std::string g_normalizeWarnings;

void addNormalizeWarning(std::string msg) {
    if (msg.empty()) return;
    if (g_normalizeWarnings.empty()) {
        g_normalizeWarnings = std::move(msg);
    } else {
        g_normalizeWarnings += "; " + msg;
    }
}

// 读出即清空（消费一次）：调用方拿到本次 parseAndValidateSettingsDoc 的全部规范化告警。
std::string takeNormalizeWarnings() {
    std::string out = std::move(g_normalizeWarnings);
    g_normalizeWarnings.clear();
    return out;
}

// ── FACE-CONFIG-017：log 段 schema 上界的**镜像常量**（只用于「越界值不可送入整数转换」的护栏）──
// 为什么需要：规范化发生在 schema 校验**之前**，此时 doc 里的数值未经任何约束；对负数或超范围的
//   JSON number 做 static_cast 是未定义行为。故只对「明确落在 schema 合法区间内」的值做就地修正，
//   区间外的值一律**不动**，交回既有 schema 判据（保持「既有非法值仍被拒」的语义不被放宽）。
// ⚠️ 若 JsonSchemaValidator.cpp 的 log 段上界改了，这两个镜像必须同步（否则本闸门会漏纳或误纳）。
constexpr double kLogSchemaMaxFileBytes = 1073741824.0;  // = schema 的 maxFileBytes.maximum（1 GiB）
constexpr double kLogSchemaMaxRollFiles = 1000.0;        // = schema 的 maxRollFiles.maximum

bool readFileAll(const std::filesystem::path& p, std::string& out, std::string& err) {
    err.clear();
    out.clear();
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) {
        err = "无法读取文件: " + p.string();
        return false;
    }
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    if (!ec && sz > kMaxConfigFileBytes) {
        err = "文件超过大小上限 (" + std::to_string(kMaxConfigFileBytes / 1024 / 1024) + "MB)";
        return false;
    }
    if (!ec) {
        std::string data;
        data.resize(static_cast<std::size_t>(sz));
        ifs.read(data.data(), static_cast<std::streamsize>(data.size()));
        out = std::move(data);
    } else {
        // Fallback for special files where file_size fails
        std::ostringstream ss;
        ss << ifs.rdbuf();
        out = ss.str();
        if (out.size() > kMaxConfigFileBytes) {
            err = "文件超过大小上限";
            return false;
        }
    }
    return true;
}

bool writeFileAll(const std::filesystem::path& p, const std::string& data, std::string& err) {
    err.clear();
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        err = "无法写入文件: " + p.string();
        return false;
    }
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!ofs.good()) {
        err = "写入失败: " + p.string();
        return false;
    }
    return true;
}

bool ensureParentDir(const std::filesystem::path& p, std::string& err) {
    err.clear();
    const auto dir = p.parent_path();
    if (dir.empty()) return true;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        err = "无法创建目录: " + dir.string();
        return false;
    }
    return true;
}

bool moveReplaceWriteThrough(const std::filesystem::path& src, const std::filesystem::path& dst, std::string& err) {
#ifdef _WIN32
    err.clear();
    if (MoveFileExW(src.wstring().c_str(), dst.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        err = "MoveFileExW(REPLACE|WRITE_THROUGH) 失败";
        return false;
    }
    return true;
#else
    (void)src;
    (void)dst;
    err = "平台不支持原子替换";
    return false;
#endif
}

// 保全证据：把无法装载的 config.json 改名另存。命名对齐 Java 侧
// FeatureTemplateEncryptedStore.quarantine：<原文件名>.corrupt.<毫秒时间戳>，同目录。
// ⚠️ 与 Java 版有一处**语义差异，勿想当然**：Java 的 File.renameTo 在 Windows 上走 MoveFileW，
//    目标已存在时**失败**；而 C++ 的 std::filesystem::rename 按标准**替换**已存在的非目录文件
//    （MSVC 用 MoveFileExW + MOVEFILE_REPLACE_EXISTING）。所以「不覆盖更早的隔离证据」**不是**
//    rename 给的，而是下面那句 `exists(dst) → continue` 预探给的——**不要删掉它**。
// 另一处差异（有意更严）：失败必须回报调用方（Java 版忽略 renameTo 返回值），调用方据此做到
//    「隔离失败时不落盘默认值」。
// 返回 false 时 movedTo 无意义，err 说明原因。
bool quarantineConfigFile(const std::filesystem::path& p, std::filesystem::path& movedTo, std::string& err) {
    err.clear();
    movedTo.clear();
    const auto dir = p.parent_path();
    const std::string baseName = p.filename().string();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    std::error_code ec;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::filesystem::path dst = dir / (baseName + ".corrupt." + std::to_string(ms + attempt));
        // 先探存在性：本平台 rename 会**覆盖**已存在目标（与 Java 的 renameTo 相反），
        // 故必须自行探测，否则会销毁更早的隔离证据；冲突则换名重试。
        if (std::filesystem::exists(dst, ec)) continue;
        std::filesystem::rename(p, dst, ec);
        if (!ec) {
            movedTo = dst;
            return true;
        }
        err = "重命名失败: " + ec.message();
        return false;
    }
    err = "无法为隔离文件分配唯一名称（时间戳冲突）";
    return false;
}

// 落盘前置闸门：序列化产物必须能通过 settings schema。
// 为什么必须有：密钥不可用（config.key.dpapi 损坏 / 跨用户复制 / DPAPI 不可用）时，
//   buildSettingsJson 返回 {"schemaVersion":1,"_error":"..."} 错误 stub。它一旦落盘，
//   下次启动必被 schema 拒 ⇒ 再隔离一次、再写同一份 stub ⇒ 隔离文件无清理地累积，
//   且 health 的 configStatus 永久 rejected。拒写非法产物即打断该自持续循环
//   （下次启动 config.json 不存在，走正常分支）。
// whyNot 回传具体原因，供 warnOut 做「失败要有可见性」输出。
static bool serializedDocIsWritable(const std::string& pretty, std::string& whyNot) {
    whyNot.clear();
    JsonValue doc;
    std::string perr;
    if (!parseJson(pretty, doc, perr)) {
        whyNot = "序列化产物不是合法 JSON: " + perr;
        return false;
    }
    std::string serr;
    if (!JsonSchemaValidator::validate(JsonSchemaValidator::buildSettingsDocSchema(), doc, &serr)) {
        whyNot = "序列化产物未通过 schema: " + serr;
        return false;
    }
    return true;
}

// --- 加解密字段格式 ---
static bool parseEncryptedStringObject(const JsonValue& v, AesGcmCiphertext& ct, std::string& err) {
    err.clear();
    if (!v.isObject()) {
        err = "期望加密对象";
        return false;
    }
    const JsonValue* nonce = v.find("nonce");
    const JsonValue* ciph = v.find("ciphertext");
    const JsonValue* tag = v.find("tag");
    if (!nonce || !ciph || !tag || !nonce->isString() || !ciph->isString() || !tag->isString()) {
        err = "加密对象缺少 nonce/ciphertext/tag";
        return false;
    }
    if (!base64Decode(nonce->s, ct.nonce)) {
        err = "nonce base64 解码失败";
        return false;
    }
    if (!base64Decode(ciph->s, ct.ciphertext)) {
        err = "ciphertext base64 解码失败";
        return false;
    }
    if (!base64Decode(tag->s, ct.tag)) {
        err = "tag base64 解码失败";
        return false;
    }
    return true;
}

static JsonValue buildEncryptedStringObject(const AesGcmCiphertext& ct, std::string& err) {
    err.clear();
    JsonValue o = JsonValue::makeObject();
    o.o["v"] = JsonValue::makeNumber(1);
    o.o["alg"] = JsonValue::makeString("AES-256-GCM");
    std::string b64;
    if (!base64Encode(ct.nonce, b64)) {
        err = "nonce base64 编码失败";
        return JsonValue{};
    }
    o.o["nonce"] = JsonValue::makeString(b64);
    if (!base64Encode(ct.ciphertext, b64)) {
        err = "ciphertext base64 编码失败";
        return JsonValue{};
    }
    o.o["ciphertext"] = JsonValue::makeString(b64);
    if (!base64Encode(ct.tag, b64)) {
        err = "tag base64 编码失败";
        return JsonValue{};
    }
    o.o["tag"] = JsonValue::makeString(b64);
    return o;
}

// 剔除脱敏回写哨兵（kMaskedSentinel="***"）：GET 输出中敏感字段以 "***" 占位，
// 客户端全量回写会把 "***" 原样带回；若直接 deepMerge 会覆盖 base 中的真实密文，
// 且校验仅拦 isObject() 放过字符串，buildSettingsJson 重加密后密文永久丢失
// （S3 CONFIRMED：脱敏全量回写不得销毁密文）。
// 因此哨兵语义 = "不修改该字段"：合并前从 patch 中剔除该键，等价于保留 base 既有值；
// 递归处理嵌套对象，保证任意层级的脱敏字段都走同一保护。
void stripMaskedSentinels(JsonValue& obj) {
    if (!obj.isObject()) return;
    for (auto it = obj.o.begin(); it != obj.o.end();) {
        if (it->second.isString() && it->second.s == kMaskedSentinel) {
            it = obj.o.erase(it);
        } else {
            stripMaskedSentinels(it->second);
            ++it;
        }
    }
}

}  // namespace

WinJsonConfigStore::WinJsonConfigStore() {
    configPath_ = defaultConfigPath();
    keyPath_ = defaultKeyPath(configPath_);
    bakPath_ = defaultBakPath(configPath_);
}

WinJsonConfigStore::~WinJsonConfigStore() {
    stopWatching();
}

std::filesystem::path WinJsonConfigStore::defaultConfigPath() {
    const std::wstring envPath = getEnvW(L"RK_WCFR_CONFIG");
    if (!envPath.empty()) {
        return std::filesystem::path(envPath);
    }
    const std::wstring appdata = getEnvW(L"APPDATA");
    std::filesystem::path base = appdata.empty() ? std::filesystem::current_path() : std::filesystem::path(appdata);
    base /= kConfigDirNameW;
    return base / L"config.json";
}

std::filesystem::path WinJsonConfigStore::defaultKeyPath(const std::filesystem::path& cfgPath) {
    return cfgPath.parent_path() / L"config.key.dpapi";
}

std::filesystem::path WinJsonConfigStore::defaultBakPath(const std::filesystem::path& cfgPath) {
    return cfgPath.parent_path() / L"config.json.bak";
}

bool WinJsonConfigStore::ensureOrLoadKey(std::vector<std::uint8_t>& keyOut, std::string& outErr) const {
    outErr.clear();
    keyOut.clear();

    std::string ioErr;
    if (std::filesystem::exists(keyPath_)) {
        std::string b64;
        if (!readFileAll(keyPath_, b64, ioErr)) {
            outErr = ioErr;
            return false;
        }
        std::vector<std::uint8_t> protectedBin;
        if (!base64Decode(b64, protectedBin)) {
            outErr = "密钥文件 base64 解码失败";
            return false;
        }
        if (!dpapiUnprotect(protectedBin, keyOut, outErr)) return false;
        if (keyOut.size() != 32) {
            outErr = "密钥长度异常（期望 32 bytes）";
            keyOut.clear();
            return false;
        }
        return true;
    }

    // 生成新密钥并落盘（DPAPI 保护）
    if (!randomBytes(32, keyOut, outErr)) return false;
    std::vector<std::uint8_t> protectedBin;
    if (!dpapiProtect(keyOut, protectedBin, outErr)) return false;
    std::string b64;
    if (!base64Encode(protectedBin, b64)) {
        outErr = "密钥 base64 编码失败";
        return false;
    }
    if (!ensureParentDir(keyPath_, ioErr)) {
        outErr = ioErr;
        return false;
    }
    if (!writeFileAll(keyPath_, b64, ioErr)) {
        outErr = ioErr;
        return false;
    }
    return true;
}

static JsonValue toSettingsDocObject(const AppConfig& cfg, bool redacted, bool encryptSensitive,
                                    const std::vector<std::uint8_t>* keyOpt, std::string& errOut) {
    errOut.clear();

    JsonValue root = JsonValue::makeObject();
    root.o["schemaVersion"] = JsonValue::makeNumber(1);

    // camera
    {
        JsonValue cam = JsonValue::makeObject();
        cam.o["preferredDeviceId"] = JsonValue::makeString(utf8FromWide(cfg.camera.preferredDeviceId));
        cam.o["width"] = JsonValue::makeNumber(cfg.camera.width);
        cam.o["height"] = JsonValue::makeNumber(cfg.camera.height);
        cam.o["fps"] = JsonValue::makeNumber(cfg.camera.fps);
        root.o["camera"] = std::move(cam);
    }

    // recognition
    {
        JsonValue r = JsonValue::makeObject();
        r.o["cascadePath"] = JsonValue::makeString(cfg.recognition.cascadePath.string());
        r.o["databasePath"] = JsonValue::makeString(cfg.recognition.databasePath.string());
        r.o["arcFaceModelPath"] = JsonValue::makeString(cfg.recognition.arcFaceModelPath.string());
        // ArcFace 节点名（FACE-ARCWIN-001）：非路径，原样序列化（不做 resolvePathFromExeDir）。
        // 默认空串，语义 = OpenCV 自动以首个输入/输出节点兜底。
        r.o["arcFaceInputName"] = JsonValue::makeString(cfg.recognition.arcFaceInputName);
        r.o["arcFaceOutputName"] = JsonValue::makeString(cfg.recognition.arcFaceOutputName);
        r.o["minFaceSizePx"] = JsonValue::makeNumber(cfg.recognition.minFaceSizePx);
        // LBPH 阈值序列化点（FACE-ARCWIN-010）：**出口也钳**，理由与 inference.intervalMs 处一致——
        //   序列化产物要过 `serializedDocIsWritable` 闸门（落盘前置条件），若 cfg 里带着越界值被写出，
        //   该闸门会失败 ⇒ 走上「不落盘 / 回退 .bak」臂；钳制使落盘产物**恒**满足运行时 schema。
        //   钳是幂等的 ⇒ 对已合法值零影响（健康装载的默认 55.0 原样通过，roundtrip 逐字符相等不变量保持）。
        //   注：本行必须保持「该键**唯一一次**赋值字面量」——config-contract.test.js 的识别段切片
        //   按下标该形态的字面量计数（重复出现会因「字面量数 ≠ 去重键数」判红），故钳制就地内联。
        r.o["identifyThreshold"] = JsonValue::makeNumber(clampIdentifyThreshold(cfg.recognition.identifyThreshold));
        // ArcFace 专用阈值（余弦相似度语义，FACE-ARCWIN-006/007）。与 LBPH 的 identifyThreshold 量纲不同，
        // 必须独立成键；后者保持 LBPH 语义不动（迁移安全）。
        r.o["identifyThresholdArcFace"] = JsonValue::makeNumber(cfg.recognition.identifyThresholdArcFace);
        r.o["enrollSamples"] = JsonValue::makeNumber(cfg.recognition.enrollSamples);
        root.o["recognition"] = std::move(r);
    }

    // inference
    {
        JsonValue i = JsonValue::makeObject();
        i.o["throttleMode"] = JsonValue::makeString(normalizeInferenceThrottleMode(cfg.inference.throttleMode));
        i.o["intervalMs"] = JsonValue::makeNumber(clampInferenceIntervalMs(cfg.inference.intervalMs));
        root.o["inference"] = std::move(i);
    }

    // dnn
    {
        JsonValue d = JsonValue::makeObject();
        d.o["enable"] = JsonValue::makeBool(cfg.dnn.enable);
        d.o["modelPath"] = JsonValue::makeString(cfg.dnn.modelPath.string());
        d.o["configPath"] = JsonValue::makeString(cfg.dnn.configPath.string());
        d.o["inputWidth"] = JsonValue::makeNumber(cfg.dnn.inputWidth);
        d.o["inputHeight"] = JsonValue::makeNumber(cfg.dnn.inputHeight);
        d.o["scale"] = JsonValue::makeNumber(cfg.dnn.scale);
        d.o["meanB"] = JsonValue::makeNumber(cfg.dnn.meanB);
        d.o["meanG"] = JsonValue::makeNumber(cfg.dnn.meanG);
        d.o["meanR"] = JsonValue::makeNumber(cfg.dnn.meanR);
        d.o["swapRB"] = JsonValue::makeBool(cfg.dnn.swapRB);
        d.o["confThreshold"] = JsonValue::makeNumber(cfg.dnn.confThreshold);
        d.o["backend"] = JsonValue::makeNumber(cfg.dnn.backend);
        d.o["target"] = JsonValue::makeNumber(cfg.dnn.target);
        root.o["dnn"] = std::move(d);
    }

    // model
    {
        JsonValue md = JsonValue::makeObject();
        md.o["detection"] = JsonValue::makeString(cfg.model.detection);
        md.o["recognition"] = JsonValue::makeString(cfg.model.recognition);
        md.o["backend"] = JsonValue::makeString(rk_accel::normalizeBackendName(cfg.model.backend, "opencv_dnn"));
        md.o["detectorBackend"] = JsonValue::makeString(rk_accel::normalizeBackendName(cfg.model.detectorBackend, cfg.model.backend));
        md.o["recognitionBackend"] = JsonValue::makeString(rk_accel::normalizeBackendName(cfg.model.recognitionBackend, cfg.model.backend));
        md.o["autoFallback"] = JsonValue::makeBool(cfg.model.autoFallback);
        md.o["int8Enabled"] = JsonValue::makeBool(cfg.model.int8Enabled);
        root.o["model"] = std::move(md);
    }

    // http
    {
        JsonValue h = JsonValue::makeObject();
        h.o["enable"] = JsonValue::makeBool(cfg.http.enable);
        h.o["port"] = JsonValue::makeNumber(cfg.http.port);
        root.o["http"] = std::move(h);
    }

    // poster（敏感字段：postUrl）
    {
        JsonValue p = JsonValue::makeObject();
        p.o["enable"] = JsonValue::makeBool(cfg.poster.enable);

        if (redacted) {
            // 脱敏输出：避免把 URL 中可能携带的 token/凭证泄漏到前端/日志。
            // 哨兵值须与 stripMaskedSentinels 的回写保护配对（"***" 回写=不修改）。
            p.o["postUrl"] = JsonValue::makeString(kMaskedSentinel);
        } else if (encryptSensitive && keyOpt) {
            const std::string plain = cfg.poster.postUrl;
            std::vector<std::uint8_t> plainBin(plain.begin(), plain.end());
            AesGcmCiphertext ct;
            std::string e;
            // ⚠️ **空明文**（= "经 UI 清空 postUrl" 落盘后的形态）依赖一个 MSVC/BCrypt 特例：
            //   空 vector 的 data() 为 nullptr，而 aes256gcmEncrypt 把它作为 pbInput 传入；CNG 的
            //   「pbInput 与 pbOutput **同为 NULL** ⇒ 按零长度认证加密处理、tag 经 pPaddingInfo 返回」
            //   特例使加密成立（否则会落进 size-query 分支、tag 保持全零 ⇒ 重启解密失败 ⇒ 整份 config
            //   被隔离 ⇒ **清空被静默撤销**）。该依赖**不是**代码显式追求的，故由
            //   tests/win 的 test_config_empty_post_url_round_trips_and_survives_reload 实测钉住
            //   （它是这条路径的**唯一**判据）；若该用例在别的 CRT/BCrypt 版本上变红，请先修这里
            //   （改为传一个非空哑指针），不要改测试。
            if (!aes256gcmEncrypt(*keyOpt, plainBin, ct, e, "poster.postUrl")) {
                errOut = e;
                return JsonValue{};
            }
            JsonValue enc = buildEncryptedStringObject(ct, e);
            if (!e.empty()) {
                errOut = e;
                return JsonValue{};
            }
            p.o["postUrl"] = std::move(enc);
        } else {
            // 允许明文（兼容/调试），但默认通过 encryptSensitive=true 落盘。
            p.o["postUrl"] = JsonValue::makeString(cfg.poster.postUrl);
        }

        p.o["throttleMs"] = JsonValue::makeNumber(cfg.poster.throttleMs);
        p.o["backoffMinMs"] = JsonValue::makeNumber(cfg.poster.backoffMinMs);
        p.o["backoffMaxMs"] = JsonValue::makeNumber(cfg.poster.backoffMaxMs);
        root.o["poster"] = std::move(p);
    }

    // log
    {
        JsonValue l = JsonValue::makeObject();
        l.o["logDir"] = JsonValue::makeString(cfg.log.logDir.string());
        // FACE-CONFIG-017：序列化点同样按总预算的**单份均分额**钳制（同 identifyThreshold 的「出口也钳」
        //   理由——落盘前置闸门 serializedDocIsWritable 要求产物能过运行时 schema，且磁盘占用由这两个值
        //   的乘积（**再乘文件集份数** kLogFileSetCount：recognition.csv / recognition.jsonl /
        //   event_log.jsonl / render_metrics.jsonl —— 末者**仅**旧版 UI 目标 win_camera_face_recognition 写）
        //   决定，故序列化产物必须是**已钳**的）。先钳 maxFileBytes，再用钳后值反推 maxRollFiles 上界。
        const std::uint64_t logMaxFileBytes = clampLogMaxFileBytes(cfg.log.maxFileBytes);
        l.o["maxFileBytes"] = JsonValue::makeNumber(static_cast<double>(logMaxFileBytes));
        l.o["maxRollFiles"] = JsonValue::makeNumber(clampLogMaxRollFiles(logMaxFileBytes, cfg.log.maxRollFiles));
        root.o["log"] = std::move(l);
    }

    // ui
    {
        JsonValue u = JsonValue::makeObject();
        u.o["windowWidth"] = JsonValue::makeNumber(cfg.ui.windowWidth);
        u.o["windowHeight"] = JsonValue::makeNumber(cfg.ui.windowHeight);
        u.o["previewScaleMode"] = JsonValue::makeNumber(cfg.ui.previewScaleMode);
        root.o["ui"] = std::move(u);
    }

    // display
    {
        JsonValue d = JsonValue::makeObject();
        d.o["outputIndex"] = JsonValue::makeNumber(cfg.display.outputIndex);
        d.o["width"] = JsonValue::makeNumber(cfg.display.width);
        d.o["height"] = JsonValue::makeNumber(cfg.display.height);
        d.o["refreshNumerator"] = JsonValue::makeNumber(cfg.display.refreshNumerator);
        d.o["refreshDenominator"] = JsonValue::makeNumber(cfg.display.refreshDenominator);
        d.o["vsync"] = JsonValue::makeBool(cfg.display.vsync);
        d.o["swapchainBuffers"] = JsonValue::makeNumber(cfg.display.swapchainBuffers);
        d.o["fullscreen"] = JsonValue::makeBool(cfg.display.fullscreen);
        d.o["allowSystemModeSwitch"] = JsonValue::makeBool(cfg.display.allowSystemModeSwitch);
        d.o["enableSRGB"] = JsonValue::makeBool(cfg.display.enableSRGB);
        d.o["gamma"] = JsonValue::makeNumber(cfg.display.gamma);
        d.o["colorTempK"] = JsonValue::makeNumber(cfg.display.colorTempK);
        d.o["aaSamples"] = JsonValue::makeNumber(cfg.display.aaSamples);
        d.o["anisoLevel"] = JsonValue::makeNumber(cfg.display.anisoLevel);
        root.o["display"] = std::move(d);
    }

    // acceleration
    {
        JsonValue a = JsonValue::makeObject();
        a.o["enableOpenCL"] = JsonValue::makeBool(cfg.acceleration.enableOpenCL);
        a.o["enableLibyuv"] = JsonValue::makeBool(cfg.acceleration.enableLibyuv);
        a.o["enableMpp"] = JsonValue::makeBool(cfg.acceleration.enableMpp);
        a.o["enableQualcomm"] = JsonValue::makeBool(cfg.acceleration.enableQualcomm);
        root.o["acceleration"] = std::move(a);
    }

    return root;
}

// 使用默认值构造配置；随后从 JSON doc 覆盖（保证缺字段时仍可运行）。
static AppConfig defaultAppConfig() {
    AppConfig cfg;
    // 路径默认与旧 INI 兼容：相对路径以 exeDir 为基准（见 WinConfig::resolvePathFromExeDir）
    cfg.recognition.cascadePath = resolvePathFromExeDir(L"assets/lbpcascade_frontalface.xml");
    cfg.recognition.databasePath = resolvePathFromExeDir(L"storage/win_face_db.yml");
    cfg.recognition.arcFaceModelPath = resolvePathFromExeDir(L"models/arcface_w600k_r50.onnx");
    cfg.dnn.modelPath = resolvePathFromExeDir(L"storage/models/opencv_face_detector_uint8.pb");
    cfg.dnn.configPath = resolvePathFromExeDir(L"storage/models/opencv_face_detector.pbtxt");
    cfg.log.logDir = resolvePathFromExeDir(L"storage/win_logs");
    return cfg;
}

static bool getNumber(const JsonValue& o, const char* k, double& out) {
    const JsonValue* v = o.find(k);
    if (!v || !v->isNumber()) return false;
    out = v->n;
    return true;
}

static bool getBool(const JsonValue& o, const char* k, bool& out) {
    const JsonValue* v = o.find(k);
    if (!v || !v->isBool()) return false;
    out = v->b;
    return true;
}

static bool getString(const JsonValue& o, const char* k, std::string& out) {
    const JsonValue* v = o.find(k);
    if (!v || !v->isString()) return false;
    out = v->s;
    return true;
}

// 键级规范化（FACE-ARCWIN-006/007 迁移过渡 + FACE-ARCWIN-010 阈值钳制 / FACE-CONFIG-017 预算钳制 /
// FACE-CONFIG-018 路径告警）。定义见本文件 parseAndValidateSettingsDoc 之后。
// 前置声明置于此，仅为保持定义靠近其唯一调用点之外的既有函数布局（定义体含 doc.find 字面量，
// 放在本处会前移既有静态切片测试的识别段起点）。
static void normalizeRecognitionKeys(JsonValue& doc);
static void normalizeLogBudgetKeys(JsonValue& doc);
static void warnUnsafeConfigPaths(JsonValue& doc);

bool WinJsonConfigStore::parseAndValidateSettingsDoc(const std::string& jsonText, AppConfig& cfgOut, std::string& outErr) const {
    outErr.clear();
    JsonValue doc;
    std::string perr;
    // 规范化告警通道：本函数是唯一的生产者，每次进入先清空 ⇒ 调用方读到的必是**本次**调用的告警
    //   （不会串味到上一次解析）。解析失败等提前 return 同样已清空。
    (void)takeNormalizeWarnings();
    if (!parseJson(jsonText, doc, perr)) {
        outErr = "JSON 解析失败: " + perr;
        return false;
    }
    // 迁移过渡（FACE-ARCWIN-006/007）：先补齐 recognition 段缺键，再做 required 收束后的 schema 校验。
    // 否则既有缺 identifyThresholdArcFace 的 config.json 会被拒，进而降级为默认值运行（历史行为更糟：
    // 穿透 INI 分支覆盖用户配置；FACE-CONFIG-001 已改为先隔离坏文件再以默认值启动）。
    normalizeRecognitionKeys(doc);
    // FACE-CONFIG-017：log 段写放大总预算 —— 必须在 schema 校验**前**就地钳制，理由见其定义处。
    normalizeLogBudgetKeys(doc);
    // FACE-CONFIG-018：6 个路径字段的强化校验 —— 本处**只告警**（见其定义处的红线说明）；
    //   真正的「拒绝」只发生在 PUT 路径（updateFromJsonBody 对 patch 内显式携带的写型字段）。
    warnUnsafeConfigPaths(doc);
    std::string schemaErr;
    if (!JsonSchemaValidator::validate(JsonSchemaValidator::buildSettingsDocSchema(), doc, &schemaErr)) {
        outErr = "Schema 校验失败: " + schemaErr;
        return false;
    }
    // poster.postUrl：若为 object，必须满足 encrypted-object 结构
    if (const JsonValue* poster = doc.find("poster"); poster && poster->isObject()) {
        if (const JsonValue* pu = poster->find("postUrl"); pu && pu->isObject()) {
            const JsonValue encSchema = JsonSchemaValidator::buildEncryptedStringSchema();
            std::string encErr;
            if (!JsonSchemaValidator::validate(encSchema, *pu, &encErr)) {
                outErr = "/poster/postUrl 加密对象校验失败: " + encErr;
                return false;
            }
            if (const JsonValue* alg = pu->find("alg"); alg && alg->isString()) {
                if (alg->s != "AES-256-GCM") {
                    outErr = "/poster/postUrl/alg: 仅支持 AES-256-GCM";
                    return false;
                }
            }
        }
    }

    // 解密敏感字段（如果是密文对象）
    std::vector<std::uint8_t> key;
    std::string kerr;
    if (!ensureOrLoadKey(key, kerr)) {
        outErr = "加载密钥失败: " + kerr;
        return false;
    }

    AppConfig cfg = defaultAppConfig();
    cfg.configPath = configPath_;

    // camera
    if (const JsonValue* cam = doc.find("camera"); cam && cam->isObject()) {
        std::string s;
        if (getString(*cam, "preferredDeviceId", s)) cfg.camera.preferredDeviceId = wideFromUtf8(s);
        double v = 0;
        if (getNumber(*cam, "width", v)) cfg.camera.width = static_cast<int>(v);
        if (getNumber(*cam, "height", v)) cfg.camera.height = static_cast<int>(v);
        if (getNumber(*cam, "fps", v)) cfg.camera.fps = static_cast<int>(v);
    }

    // recognition
    if (const JsonValue* r = doc.find("recognition"); r && r->isObject()) {
        std::string s;
        double v = 0;
        if (getString(*r, "cascadePath", s)) cfg.recognition.cascadePath = resolvePathFromExeDir(s);
        if (getString(*r, "databasePath", s)) cfg.recognition.databasePath = resolvePathFromExeDir(s);
        if (getString(*r, "arcFaceModelPath", s)) cfg.recognition.arcFaceModelPath = resolvePathFromExeDir(s);
        // 节点名不是路径：原样读入，绝不做 resolvePathFromExeDir。
        // getString 仅要求"键存在且为字符串"（空串亦满足），故显式置空可覆盖 cfg 的旧值；
        // 键缺失时才保留默认（空串）。
        if (getString(*r, "arcFaceInputName", s)) cfg.recognition.arcFaceInputName = s;
        if (getString(*r, "arcFaceOutputName", s)) cfg.recognition.arcFaceOutputName = s;
        if (getNumber(*r, "minFaceSizePx", v)) cfg.recognition.minFaceSizePx = static_cast<int>(v);
        // LBPH 阈值（FACE-ARCWIN-010）**只此一处读取**；此点**不重复钳制**的理由：本函数入口的
        // normalizeRecognitionKeys 已在 schema 校验前把该键钳进 [1, 64]，schema 又保证它必须存在且
        // 落在同区间 ⇒ 这里读到的必是已钳值（重复钳制是死代码，与「极简」相悖）。
        // 上面三项路径字段的读取同理不在此处判定（路径校验在 warnUnsafeConfigPaths 内对同一个 doc 做，
        // 判据与 PUT 路径完全同源）。
        if (getNumber(*r, "identifyThreshold", v)) cfg.recognition.identifyThreshold = v;
        // ArcFace 专用阈值（余弦相似度）。键缺失时保留默认 0.45。
        if (getNumber(*r, "identifyThresholdArcFace", v)) cfg.recognition.identifyThresholdArcFace = v;
        if (getNumber(*r, "enrollSamples", v)) cfg.recognition.enrollSamples = static_cast<int>(v);
    }

    // inference
    if (const JsonValue* i = doc.find("inference"); i && i->isObject()) {
        std::string s;
        double v = 0;
        if (getString(*i, "throttleMode", s)) cfg.inference.throttleMode = normalizeInferenceThrottleMode(std::move(s));
        if (getNumber(*i, "intervalMs", v)) cfg.inference.intervalMs = clampInferenceIntervalMs(static_cast<int>(v));
    }

    // dnn
    if (const JsonValue* d = doc.find("dnn"); d && d->isObject()) {
        std::string s;
        double v = 0;
        bool b = false;
        if (getBool(*d, "enable", b)) cfg.dnn.enable = b;
        if (getString(*d, "modelPath", s)) cfg.dnn.modelPath = resolvePathFromExeDir(s);
        if (getString(*d, "configPath", s)) cfg.dnn.configPath = resolvePathFromExeDir(s);

        // 冗余 env 覆盖（FACE-CONFIG-015）：与函数末尾同名覆盖重复，且同样会被烤进磁盘（见末尾登记块）。
        const std::wstring envModel = getEnvW(L"RK_WCFR_DNN_MODEL");
        if (!envModel.empty()) cfg.dnn.modelPath = resolvePathFromExeDir(envModel);

        const std::wstring envConfig = getEnvW(L"RK_WCFR_DNN_CONFIG");
        if (!envConfig.empty()) cfg.dnn.configPath = resolvePathFromExeDir(envConfig);

        if (getNumber(*d, "inputWidth", v)) cfg.dnn.inputWidth = static_cast<int>(v);
        if (getNumber(*d, "inputHeight", v)) cfg.dnn.inputHeight = static_cast<int>(v);
        if (getNumber(*d, "scale", v)) cfg.dnn.scale = v;
        if (getNumber(*d, "meanB", v)) cfg.dnn.meanB = static_cast<int>(v);
        if (getNumber(*d, "meanG", v)) cfg.dnn.meanG = static_cast<int>(v);
        if (getNumber(*d, "meanR", v)) cfg.dnn.meanR = static_cast<int>(v);
        if (getBool(*d, "swapRB", b)) cfg.dnn.swapRB = b;
        if (getNumber(*d, "confThreshold", v)) cfg.dnn.confThreshold = v;
        if (getNumber(*d, "backend", v)) cfg.dnn.backend = static_cast<int>(v);
        if (getNumber(*d, "target", v)) cfg.dnn.target = static_cast<int>(v);
    }

    // model (optional, silently fall back to defaults if missing)
    if (const JsonValue* m = doc.find("model"); m && m->isObject()) {
        std::string s;
        if (getString(*m, "detection", s)) cfg.model.detection = s;
        if (getString(*m, "recognition", s)) cfg.model.recognition = s;
        if (getString(*m, "backend", s)) cfg.model.backend = rk_accel::normalizeBackendName(s, cfg.model.backend);
        if (getString(*m, "detectorBackend", s)) cfg.model.detectorBackend = rk_accel::normalizeBackendName(s, cfg.model.backend);
        if (getString(*m, "recognitionBackend", s)) cfg.model.recognitionBackend = rk_accel::normalizeBackendName(s, cfg.model.backend);
        bool b = false;
        if (getBool(*m, "autoFallback", b)) cfg.model.autoFallback = b;
        if (getBool(*m, "int8Enabled", b)) cfg.model.int8Enabled = b;
        if (cfg.model.detectorBackend.empty()) cfg.model.detectorBackend = cfg.model.backend;
        if (cfg.model.recognitionBackend.empty()) cfg.model.recognitionBackend = cfg.model.backend;
    }

    // http
    if (const JsonValue* h = doc.find("http"); h && h->isObject()) {
        bool b = false;
        double v = 0;
        if (getBool(*h, "enable", b)) cfg.http.enable = b;
        if (getNumber(*h, "port", v)) cfg.http.port = static_cast<int>(v);

    }

    // poster
    if (const JsonValue* p = doc.find("poster"); p && p->isObject()) {
        bool b = false;
        double v = 0;
        if (getBool(*p, "enable", b)) cfg.poster.enable = b;
        if (const JsonValue* pu = p->find("postUrl"); pu) {
            if (pu->isString()) {
                cfg.poster.postUrl = pu->s;
            } else if (pu->isObject()) {
                AesGcmCiphertext ct;
                std::string e;
                if (!parseEncryptedStringObject(*pu, ct, e)) {
                    outErr = "postUrl 加密对象解析失败: " + e;
                    return false;
                }
                std::vector<std::uint8_t> plain;
                if (!aes256gcmDecrypt(key, ct, plain, e, "poster.postUrl")) {
                    outErr = "postUrl 解密失败: " + e;
                    return false;
                }
                // ⚠️ 空明文（"经 UI 清空 postUrl" 落盘后就是这个形态）时 plain 为空 vector，其 data()
                //   可能是 nullptr，而标准要求 assign(const char*, n) 的指针非空（n=0 亦然）⇒ 显式判空，
                //   不依赖实现的宽容（这条路径由 tests/win 的
                //   test_config_empty_post_url_round_trips_and_survives_reload 钉住）。
                if (plain.empty()) {
                    cfg.poster.postUrl.clear();
                } else {
                    cfg.poster.postUrl.assign(reinterpret_cast<const char*>(plain.data()), plain.size());
                }
            }
        }

        // 冗余 env 覆盖（FACE-CONFIG-015）：与函数末尾同名覆盖重复，且同样会被烤进磁盘（见末尾登记块）。
        const std::wstring envUrl = getEnvW(L"RK_WCFR_POST_URL");
        if (!envUrl.empty()) cfg.poster.postUrl = utf8FromWide(envUrl);
        if (getNumber(*p, "throttleMs", v)) cfg.poster.throttleMs = static_cast<int>(v);
        if (getNumber(*p, "backoffMinMs", v)) cfg.poster.backoffMinMs = static_cast<int>(v);
        if (getNumber(*p, "backoffMaxMs", v)) cfg.poster.backoffMaxMs = static_cast<int>(v);
    }

    // log
    if (const JsonValue* l = doc.find("log"); l && l->isObject()) {
        std::string s;
        double v = 0;
        if (getString(*l, "logDir", s)) cfg.log.logDir = resolvePathFromExeDir(s);
        // FACE-CONFIG-017 的两个读取点：同 identifyThreshold 的处置理由——normalizeLogBudgetKeys 已在
        //   schema 校验前按总预算的**单份均分额**就地钳制，schema 又保证两者必须存在且落在各自区间内
        //   ⇒ 这里读到的是已钳值，其乘积**必满足**单份均分额（≤ 总预算）（在此再钳一次是死代码）。
        //   注：static_cast<std::uint64_t>(v) 的安全性由 schema 的 [1024, 1 GiB] 保证（非有限值 / 负数 /
        //   超范围值到不了这里）。
        if (getNumber(*l, "maxFileBytes", v)) cfg.log.maxFileBytes = static_cast<std::uint64_t>(v);
        if (getNumber(*l, "maxRollFiles", v)) cfg.log.maxRollFiles = static_cast<int>(v);
    }

    // ui
    if (const JsonValue* u = doc.find("ui"); u && u->isObject()) {
        double v = 0;
        if (getNumber(*u, "windowWidth", v)) cfg.ui.windowWidth = static_cast<int>(v);
        if (getNumber(*u, "windowHeight", v)) cfg.ui.windowHeight = static_cast<int>(v);
        if (getNumber(*u, "previewScaleMode", v)) cfg.ui.previewScaleMode = static_cast<int>(v);
    }

    // display
    if (const JsonValue* d = doc.find("display"); d && d->isObject()) {
        double v = 0;
        bool b = false;
        if (getNumber(*d, "outputIndex", v)) cfg.display.outputIndex = static_cast<int>(v);
        if (getNumber(*d, "width", v)) cfg.display.width = static_cast<int>(v);
        if (getNumber(*d, "height", v)) cfg.display.height = static_cast<int>(v);
        if (getNumber(*d, "refreshNumerator", v)) cfg.display.refreshNumerator = static_cast<std::uint32_t>(v);
        if (getNumber(*d, "refreshDenominator", v)) cfg.display.refreshDenominator = static_cast<std::uint32_t>(v);
        if (cfg.display.refreshDenominator == 0) cfg.display.refreshDenominator = 1;
        if (getBool(*d, "vsync", b)) cfg.display.vsync = b;
        if (getNumber(*d, "swapchainBuffers", v)) cfg.display.swapchainBuffers = static_cast<int>(v);
        if (getBool(*d, "fullscreen", b)) cfg.display.fullscreen = b;
        if (getBool(*d, "allowSystemModeSwitch", b)) cfg.display.allowSystemModeSwitch = b;
        if (getBool(*d, "enableSRGB", b)) cfg.display.enableSRGB = b;
        if (getNumber(*d, "gamma", v)) cfg.display.gamma = v;
        if (getNumber(*d, "colorTempK", v)) cfg.display.colorTempK = static_cast<int>(v);
        if (getNumber(*d, "aaSamples", v)) cfg.display.aaSamples = static_cast<int>(v);
        if (getNumber(*d, "anisoLevel", v)) cfg.display.anisoLevel = static_cast<int>(v);
    }

    // acceleration
    if (const JsonValue* a = doc.find("acceleration"); a && a->isObject()) {
        bool b = false;
        if (getBool(*a, "enableOpenCL", b)) cfg.acceleration.enableOpenCL = b;
        if (getBool(*a, "enableLibyuv", b)) cfg.acceleration.enableLibyuv = b;
        if (getBool(*a, "enableMpp", b)) cfg.acceleration.enableMpp = b;
        if (getBool(*a, "enableQualcomm", b)) cfg.acceleration.enableQualcomm = b;
    }

    // Apply environment variable overrides if present
    //
    // ⚠️ FACE-CONFIG-015（已知不对称，本轮**不修**，取方案 (b)：如实登记不修）：
    //   下面这些 RK_WCFR_* 覆盖（本块 4 个 + 上文 dnn / poster 块内 3 处冗余覆盖的 env 读取：
    //   getEnvW(L"RK_WCFR_DNN_MODEL") / (L"RK_WCFR_DNN_CONFIG") / (L"RK_WCFR_POST_URL")）会被**写回磁盘**。
    //   ⚠️ 此处**刻意不写行号**：本文件行号随每次编辑漂移（上一版写的 622/625/689 即已失准）⇒ 只按
    //   函数/字面量定位，避免留下会腐烂的引用。
    //   原因：本函数把 env 值**无条件覆盖进 cfg**，而序列化侧
    //   （toSettingsDocObject / buildSettingsJson）只看到覆盖后的 cfg、**无法回退到用户原值** ⇒ 一旦发生
    //   「热重载回滚写回」（reloadFromDisk）或 PUT（updateFromJsonBody），两者都经 parseAndValidateSettingsDoc
    //   → 序列化落盘，env 值就被**烤进 config.json**；此后**不带**该 env 启动时行为随之改变、用户原本的值被静默替换。
    //   · 这是**既有性质，非本轮引入**：PUT 路径一直如此（且 initialize 成功分支的 lastGoodJsonPretty_
    //     也由 env 覆盖后的 cfg 序列化而来，故 PUT 的合并基底亦含 env 值；见 `updateFromJsonBody` 的合并基底
    //     注释——那里的基底**含 env 覆盖值**，旧措辞「解耦环境覆盖项」不成立）。
    //   · 本轮新增的**触达路径**是「热重载被拒后**无需用户动作**即写回」（FACE-CONFIG-010，属**上一提交**、
    //     非本批新增），使该不对称更易被触发 ⇒ 值得登记。
    //   为什么**不**修（选 (b) 的理由）：修法**非平凡**——**注意**：准确说法**不是**「必然引入来源歧义」。
    //     现有序列化侧只拿到「env 覆盖后的 cfg」，无从分辨哪些字段被 env 覆盖；但存在一条**不涉及**「把用户在
    //     UI 显式改的值误判成 env 值」那类歧义的可行替代（**后续可选方案**，本轮未做）：
    //       在 parseAndValidateSettingsDoc 内把「应用 env **之前**的 cfg」留一份**来源快照**；落盘一律用该
    //       **env-free 快照**，内存 cfg_ 仍用 env-applied 值（load 时 env 照旧重放）。
    //     其代价：需把「落盘用哪份 cfg」贯穿 **initialize / PUT / 回滚** 三处写盘路径，并新增相应回归用例
    //     （非平凡、触及面广），故本轮未做。环境变量本身按文档定位为「容器化 / 临时调试」用途，该场景下
    //     env 值落盘通常可接受；留待有明确需求时实现上述替代方案。
    //   详见 docs/guides/windows-service/config.md。
    const std::wstring envModel = getEnvW(L"RK_WCFR_DNN_MODEL");
    const std::wstring envConfig = getEnvW(L"RK_WCFR_DNN_CONFIG");
    const std::wstring envPort = getEnvW(L"RK_WCFR_HTTP_PORT");
    const std::wstring envUrl = getEnvW(L"RK_WCFR_POST_URL");

    if (!envModel.empty()) cfg.dnn.modelPath = resolvePathFromExeDir(envModel);
    if (!envConfig.empty()) cfg.dnn.configPath = resolvePathFromExeDir(envConfig);
    if (!envPort.empty()) {
        try {
            const int p = std::stoi(envPort);
            if (p >= 1 && p <= 65535) cfg.http.port = p;
        } catch (...) {
        }
    }
    if (!envUrl.empty()) cfg.poster.postUrl = utf8FromWide(envUrl);

    cfgOut = std::move(cfg);
    return true;
}

// FACE-ARCWIN-006/007 迁移过渡 + FACE-ARCWIN-010 阈值钳制：recognition 段的键级规范化。
// 本函数只做两件事：(1) 补 identifyThresholdArcFace 缺键；(2) 把 identifyThreshold 钳进 [1, 64]。
//
// (1) 为什么需要补缺键：identifyThresholdArcFace 已进入 schema.required（见 JsonSchemaValidator.cpp），
// 而既有部署的 config.json 没有该键（它随本批才进入序列化）。若不先补齐，老配置会在启动校验被拒
// ⇒ 降级为默认值运行。历史行为（修复前）：initialize 在校验失败时并不 return，而是穿透到 INI 迁移分支、
// 以 INI/默认值**覆盖写 config.json**（仅留一条 warn），用户配置即被丢弃。
// 现行为（FACE-CONFIG-001）：校验被拒时先把坏配置改名另存再以默认值启动，不再覆盖销毁；但缺键的既有
// 配置仍会降级运行，故仍须在此补齐，使其走「加载成功」路径。
//
// (2) 为什么必须钳 identifyThreshold（FACE-ARCWIN-010）：本批把 schema 的该键上界从 100000 收紧到
// 64（卡方距离的物理上界）。若不在此**先**就地把越界值钳到上界，任何既有 config.json 里 > 64 的值会
// 从「合法」突变为「不合法」⇒ 启动校验失败 ⇒ 隔离 + 默认值运行（用户配置被弃用，即红线）。
// 钳制 ↔ schema 收紧必须**同批**且钳制**在前**。下界同取 1.0（与三方 schema 的 minimum 同值），
// 使 clamp 输出恒落在 schema 合法区间内 —— 详细取舍见 WinConfig.cpp 的 clampIdentifyThreshold。
//
// 边界（刻意的最小改动）：除上述两项外，不推广到 recognition 段其余键——其余 7 个键的接受/拒绝语义
// 属历史契约，本批不改。若一并补默认，「删除 cascadePath 等必填键」会从「校验失败 ⇒ 回滚 .bak」静默
// 变成「以默认值接受」——属未声明的行为放宽。补入值与解析侧「键缺失保留结构体默认」同源，故本规范化
// 不改变任何**已被接受**配置的解析结果，只让新增的 required / 收紧的 maximum 校验能通过。
static void normalizeRecognitionKeys(JsonValue& doc) {
    JsonValue* rec = doc.find("recognition");
    if (!rec || !rec->isObject()) return;
    if (!rec->find("identifyThresholdArcFace")) {
        rec->o["identifyThresholdArcFace"] =
            JsonValue::makeNumber(defaultAppConfig().recognition.identifyThresholdArcFace);
    }
    // 只处理「超过物理上界」的值 ⇒ **不**放宽下界的既有拒绝语义（< 1 的值照旧被 schema 拒，与改动前
    //   一致；只有 > 64 这一侧从「被接受」改判为「钳到 64 并告警」，属不让任何既有配置失效的**收紧**方向）。
    if (JsonValue* thr = rec->find("identifyThreshold"); thr && thr->isNumber() && thr->n > kIdentifyThresholdMax) {
        const double raw = thr->n;
        thr->n = clampIdentifyThreshold(raw);
        addNormalizeWarning("recognition.identifyThreshold=" + std::to_string(raw) +
                            " 超过 LBPH 卡方距离的物理上界 " + std::to_string(kIdentifyThresholdMax) +
                            "（超过该值时「距离 <= 阈值」恒真、阈值形同虚设），已就地钳为 " +
                            std::to_string(thr->n));
    }
}

// FACE-CONFIG-017（S-3）：log 段写放大总预算——**只钳不拒**，且必须在 schema 校验**前**完成。
// 为什么在 schema 前：落盘面**最多 4 份**文件集（recognition.csv、recognition.jsonl、**event_log.jsonl**
//   —— 后者复用同一对 cfg.log 上限与同一 logDir，本批起随配置接线 —— 与 render_metrics.jsonl
//   —— **仅**旧版 UI 目标写）各占 maxFileBytes × (maxRollFiles + 1) 字节；而 schema 本身允许单份
//   1 GiB × 1001（四份最坏约 4 TiB）⇒ 只靠 schema 拦不住磁盘耗尽。此处把两者钳进 512 MiB 总预算的
//   **均分额**（kLogPerSetBudgetBytes = 512 MiB / 4 = 134217728 字节）内，schema 校验随后照常执行
//   （**不动** schema 的 log 段上下界——收紧会让既有合法配置突然失效，即红线；故本项只做运行时
//   钳制，schema 仍允许大值 ⇒ 「配置声明值 ≠ 生效值」是本项刻意保留的残留，见报告）。
// 为什么在此**不做**「钳后仍不合法」的兜底：钳制只往下压，从不抬升下界；区间外的值（负数 / 0 /
//   超 schema 上界 / 非有限值）一律不动，交回既有 schema 判据 ⇒ 「既有非法值仍被拒」的语义不变。
static void normalizeLogBudgetKeys(JsonValue& doc) {
    JsonValue* lg = doc.find("log");
    if (!lg || !lg->isObject()) return;

    JsonValue* mfb = lg->find("maxFileBytes");
    // 只对「均分额内单文件上限 < 值 ≤ schema 上界」这一段做就地修正：落在该区间外时**不做**整数转换
    //   （对负数 / 非有限值 / 超 u64 范围的 double 做 static_cast 是未定义行为），一律交回 schema 判据。
    const bool mfbFixable = mfb && mfb->isNumber() && mfb->n > static_cast<double>(kLogMaxFileBytesCap) &&
                            mfb->n <= kLogSchemaMaxFileBytes;
    if (mfbFixable) {
        const std::uint64_t raw = static_cast<std::uint64_t>(mfb->n);
        const std::uint64_t clamped = clampLogMaxFileBytes(raw);
        if (clamped != raw) {
            mfb->n = static_cast<double>(clamped);
            addNormalizeWarning("log.maxFileBytes=" + std::to_string(raw) + " 超出日志写放大总预算（总预算 " +
                                std::to_string(kLogBudgetBytes) + " 字节由 " + std::to_string(kLogFileSetCount) +
                                " 份文件集（recognition.csv / recognition.jsonl / event_log.jsonl，以及"
                                "旧版 UI 目标的 render_metrics.jsonl）均分 ⇒ 单份上界 " +
                                std::to_string(kLogPerSetBudgetBytes) +
                                " 字节 = maxFileBytes × (maxRollFiles + 1)），已就地钳为 " +
                                std::to_string(clamped));
        }
    }

    JsonValue* mrf = lg->find("maxRollFiles");
    if (!mrf || !mrf->isNumber() || mrf->n < 1.0 || mrf->n > kLogSchemaMaxRollFiles) return;  // 区间外交回 schema
    // 用**钳后**的 maxFileBytes 反推上界（与 clampLogMaxRollFiles 的约定一致；用原值会算出偏小的上界）。
    //   maxFileBytes 缺失 / 非数 / 越出可安全转换的区间时，取「均分额内单文件上限」作为保守输入——
    //   这样转换必安全，且这些形态本就会被 schema 的 required / 上下界拒掉（本闸门不改其判定）。
    const double mfbSafe = (mfb && mfb->isNumber() && mfb->n >= 0.0 && mfb->n <= kLogSchemaMaxFileBytes)
                               ? mfb->n
                               : static_cast<double>(kLogMaxFileBytesCap);
    const std::uint64_t mfbForBudget = clampLogMaxFileBytes(static_cast<std::uint64_t>(mfbSafe));
    const int raw = static_cast<int>(mrf->n);  // 已确认落在 [1, 1000] ⇒ 转换安全
    const int clamped = clampLogMaxRollFiles(mfbForBudget, raw);
    if (clamped != raw) {
        mrf->n = clamped;
        addNormalizeWarning("log.maxRollFiles=" + std::to_string(raw) + " 超出日志写放大总预算（总预算 " +
                            std::to_string(kLogBudgetBytes) + " 字节由 " + std::to_string(kLogFileSetCount) +
                            " 份文件集均分 ⇒ 单份上界 " + std::to_string(kLogPerSetBudgetBytes) +
                            " 字节 = maxFileBytes × (maxRollFiles + 1)），已就地钳为 " +
                            std::to_string(clamped));
    }
}

// FACE-CONFIG-018（S-1）：6 个路径字段的强化校验——**本处只告警**。
// ⚠️ 红线（最高优先）：本函数在**启动期**（initialize 与热重载都经 parseAndValidateSettingsDoc）运行，
//   此处**绝不**能返回 false —— 启动校验失败 ⇒ 隔离 + 默认值运行 ⇒ 用户配置被弃用。
//   故所有路径违规在此一律降级为告警（并入 warnOut ⇒ health 的 configWarning + configStatus=degraded），
//   真正的「拒绝」只发生在 PUT 路径（updateFromJsonBody 对 patch 内**显式携带**的写型字段）。
// 为什么对读型字段也只告警、不拒绝（分阶段登记）：读型字段的误伤面与写型相同（绝对路径是官方用法），
//   而它们的风险（读到别处文件）远低于写型（create_directories + 覆写/滚动删除）；本批只做「可见化」，
//   把写型先落到 PUT 拒绝。后续若要收紧，应连同可写根白名单一起做（AppConfig 目前没有该字段）。
static void warnUnsafeConfigPaths(JsonValue& doc) {
    // 写型字段排在前：其风险含「写入/创建目录/滚动删除」，是本次加固的重点。
    struct PathField {
        const char* section;
        const char* key;
        bool writeType;
    };
    static const PathField kFields[] = {
        {"recognition", "databasePath", true},   // FaceDatabase::save：create_directories(parent) + 原子替换覆写目标
        {"log", "logDir", true},                 // StructuredLogger/EventLogger：create_directories + 写固定名文件 + 滚动 rename/remove
        {"recognition", "cascadePath", false},   // 只读
        {"recognition", "arcFaceModelPath", false},
        {"dnn", "modelPath", false},
        {"dnn", "configPath", false},
    };
    for (const PathField& f : kFields) {
        const JsonValue* sec = doc.find(f.section);
        if (!sec || !sec->isObject()) continue;
        const JsonValue* v = sec->find(f.key);
        if (!v || !v->isString()) continue;
        std::string why;
        // 路径按 JSON 里的**原样字符串**构造（与解析侧 resolvePathFromExeDir 的入参同源）：
        //   `..` 在相对与绝对两种形态下都存在于该字符串中，判别不受是否已拼接 exeDir 影响。
        if (!checkConfigPath(std::filesystem::path(v->s), PathPolicy::Warn, why)) {
            addNormalizeWarning(std::string(f.writeType ? "写型" : "读型") + "路径字段 " + f.section + "." + f.key + "：" + why);
        }
    }
}

std::string WinJsonConfigStore::buildSettingsJson(const AppConfig& cfg, bool redacted, bool encryptSensitive) const {
    std::vector<std::uint8_t> key;
    std::string err;
    if (encryptSensitive && !ensureOrLoadKey(key, err)) {
        // 返回最小 JSON；错误信息用通用消息替代以防止泄漏敏感细节
        JsonValue o = JsonValue::makeObject();
        o.o["schemaVersion"] = JsonValue::makeNumber(1);
        o.o["_error"] = JsonValue::makeString("key_error");
        return toJsonString(o, true);
    }
    JsonValue doc = toSettingsDocObject(cfg, redacted, encryptSensitive, encryptSensitive ? &key : nullptr, err);
    if (!err.empty()) {
        JsonValue o = JsonValue::makeObject();
        o.o["schemaVersion"] = JsonValue::makeNumber(1);
        o.o["_error"] = JsonValue::makeString("encrypt_error");
        return toJsonString(o, true);
    }
    return toJsonString(doc, true);
}

bool WinJsonConfigStore::writeAtomicallyWithBackup(const std::string& jsonPretty, std::string& outErr) {
    outErr.clear();
    std::string ioErr;
    if (!ensureParentDir(configPath_, ioErr)) {
        outErr = ioErr;
        return false;
    }
    // 先备份旧文件，保留最近 5 个轮转副本
    if (std::filesystem::exists(configPath_)) {
        std::error_code ec;
        // Rotate: config.json.bak.5 -> remove, .4->.5, .3->.4, .2->.3, .1->.2, .bak->.1
        auto rotatePath = [&](int n) { return configPath_.parent_path() / (L"config.json.bak." + std::to_wstring(n)); };
        std::filesystem::remove(rotatePath(5), ec);
        for (int i = 4; i >= 1; --i) {
            std::filesystem::path src = rotatePath(i);
            if (std::filesystem::exists(src, ec)) {
                std::filesystem::rename(src, rotatePath(i + 1), ec);
            }
        }
        if (std::filesystem::exists(bakPath_, ec)) {
            std::filesystem::rename(bakPath_, rotatePath(1), ec);
        }
        std::filesystem::copy_file(configPath_, bakPath_, std::filesystem::copy_options::overwrite_existing, ec);
        // 备份失败不阻断写入（但会降低回滚能力）
    }
    const std::filesystem::path tmp = configPath_.parent_path() / L"config.json.tmp";
    if (!writeFileAll(tmp, jsonPretty, ioErr)) {
        outErr = ioErr;
        return false;
    }
    if (!moveReplaceWriteThrough(tmp, configPath_, outErr)) {
        // 尝试清理 tmp
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool WinJsonConfigStore::initialize(std::string& warnOut) {
    warnOut.clear();

    // 本次启动是否因「既有 config.json 无法装载」而降级运行。两条失败路径（不可读 / 被拒）共用此标记。
    bool loadRejected = false;
    std::string quarantineName;  // 隔离成功后的文件名（非全路径）

    // FACE-CONFIG-014：清扫上一次进程崩溃残留的回滚临时文件。
    // reloadFromDisk 的回滚写回用**固定名** config.json.rollback.tmp（writeFileAll → moveReplaceWriteThrough），
    // 进程若在这两步之间崩溃会残留该文件；此前 initialize 与 writeAtomicallyWithBackup **都不清理**它。
    // 在启动清扫使其「重启即自愈」，无需人工删除。
    // 安全性（如实评估）：本类**不承诺多实例并发**（见类注释「多实例同时写会冲突」）；默认落盘目录
    //   %APPDATA%\rk_wcfr 为单用户单实例服务所用。另一进程若真在写同一目录，则同样固定名的
    //   config.json.tmp（writeAtomicallyWithBackup）本就有同样的多实例暴露 ⇒ 清理本残留并不比既有
    //   写盘更激进。误删的后果也**可控**：若删除的是一份正在被另一进程使用的回滚 tmp，对方随后的
    //   moveReplaceWriteThrough 因源文件缺失而失败 ⇒ 该次回滚「不写回」（与「无可用 .bak」臂一样安全，
    //   不会损坏 config.json）。删除仅针对**该固定名**（不递归、不遍历目录内容）⇒ 合法 config.json /
    //   .bak / .corrupt.* 均不受影响（负对照见 tests/win/test_win_json_config.cpp）。
    {
        // 本处亦持 mu_（FACE-CONFIG-014）：清扫是对**配置目录**的文件系统变更（删除），使其与同批两处
        //   initialize 写盘补锁同形态，让「对配置目录的变更统一在 mu_ 下」这条口径**可机械复核**。
        //   死锁核实（逐字）：本临界区**只**调 std::filesystem::remove（不取 mu_、不回调本类任何成员）
        //   ⇒ 无递归加锁；initialize 处于启动阶段、本无并发消费者 ⇒ 不引入新的锁竞争。故补锁安全。
        //   （清扫与随后的读取/写盘各自独立成对临界区，不嵌套 ⇒ 无锁序问题。）
        std::lock_guard<std::mutex> lock(mu_);
        std::error_code cleanEc;
        // 清理失败（残留为目录 / 被杀软占用等）不影响启动：残留无正确性影响，下次回滚以 trunc 覆盖它。
        std::filesystem::remove(configPath_.parent_path() / L"config.json.rollback.tmp", cleanEc);
    }

    // 读取现有 config.json
    if (std::filesystem::exists(configPath_)) {
        std::string txt, err;
        if (readFileAll(configPath_, txt, err)) {
            AppConfig cfg;
            if (parseAndValidateSettingsDoc(txt, cfg, err)) {
                // FACE-ARCWIN-010 / FACE-CONFIG-017 / FACE-CONFIG-018 的规范化告警**就地取走**：
                // 本处是它们**唯一用户可见**的出口——并入 warnOut ⇒ initWarning_ ⇒ /api/v1/health 的
                // configWarning + configStatus=degraded（零新增通道）。健康装载（默认值全部在界内、
                // 路径无 `..` 段）时该串为空 ⇒ 「正常启动零告警 / stderr 零输出」语义与既有用例
                // （test_config_healthy_load_not_quarantined / _silent_stderr）保持不变。
                // 必须在 parseAndValidateSettingsDoc 返回后**立刻**取走：本函数后续还会经 buildSettingsJson
                // 等路径，但都不会再触发规范化。
                const std::string normWarn = takeNormalizeWarnings();
                // 级联路径迁移：命中旧部署路径（tests/data/...）且该旧文件已不存在时改指新路径，并尝试持久化。
                // ④ 修复：这次写盘此前被 (void) 吞掉，且成功分支无条件 initWarning_.clear()，结果「迁移只改了内存、
                //   未落到磁盘」却对外报 configStatus=ok（静默）。现把失败并入 warnOut/initWarning_，使其在 health
                //   上体现为 degraded（键为 configWarning）。⚠️ migrateWarn 仅在写盘**真的失败**时才非空 ⇒ 健康
                //   装载（不触发本迁移分支）时 warnOut 仍为空，保持「正常启动零告警 / stderr 零输出」语义。
                std::string migrateWarn;
                const auto oldCascade = resolvePathFromExeDir(L"tests/data/lbpcascade_frontalface.xml");
                if (cfg.recognition.cascadePath == oldCascade && !std::filesystem::exists(cfg.recognition.cascadePath)) {
                    cfg.recognition.cascadePath = resolvePathFromExeDir(L"assets/lbpcascade_frontalface.xml");
                    const std::string prettyMigrated = buildSettingsJson(cfg, false, true);
                    std::string writeErr;
                    // ★ 承重不变量（FACE-CONFIG-013）：任何**写入 config.json 内容**的路径都必须持 mu_（见类级不变量）。本处已持。
                    //   本处是 initialize 的**迁移落盘**：启动阶段（早于 startWatching / 轮询循环）本无并发消费者，
                    //   此处持锁是**幂等加固**：使「写盘必须持锁」在每个调用点一致可见，后人新增写盘路径时照抄该形态。
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        if (!writeAtomicallyWithBackup(prettyMigrated, writeErr)) {
                            // 只改了内存 cascadePath；写盘失败 ⇒ 磁盘仍是失效的旧路径，下次启动会再次进入本分支。
                            migrateWarn = "级联路径迁移未持久化（写入 config.json 失败）: " + writeErr;
                        }
                    }
                }

                // 规范化告警并入 warnOut（顺序：迁移告警在前、规范化告警在后）。二者语义同级——
                // 都表示「本次启动**成功但降级**」，故合并进同一条 configWarning 文本。
                // ⚠️ **跨启动持续、且无自愈路径**（如实登记，本批不改行为）：本成功分支**只读不写**——
                //   它不把 cfg（含被钳后的值）回写 config.json，磁盘上的越界原值 / 含 `..` 的路径**原封
                //   不动** ⇒ 同一份「功能完全正常」的配置在**每次启动都复现同一条**告警：重启不收敛，
                //   只有一次 PUT 或手工编辑磁盘值才会消失。health 的 configStatus 也随之**长期**停在
                //   degraded（成因清单见 docs/windows-web-spa/openapi.yaml，该文档需补充本条，见报告）。
                //   与之对照：上方级联路径迁移分支在**写盘失败**时告警，那条告警带自愈路径（下次启动
                //   重试写盘），两者的「持续性」不同类。
                if (!normWarn.empty()) {
                    migrateWarn = migrateWarn.empty() ? normWarn : (migrateWarn + "; " + normWarn);
                }

                const std::string pretty = buildSettingsJson(cfg, false, true);
                std::lock_guard<std::mutex> lock(mu_);
                cfg_ = cfg;
                lastGoodJsonPretty_ = pretty;
                std::error_code ec;
                lastWriteTime_ = std::filesystem::last_write_time(configPath_, ec);
                // 诊断字段永远描述**最近一次** initialize：成功路径同样复位上一轮的 rejected / 隔离名；
                // initWarning_ 仅回填本次迁移告警（健康装载时 migrateWarn 为空 ⇒ 复位为空串），
                // 否则同一 store 二次 initialize 会残留上一轮的 rejected / warning / 隔离名。
                configRejectedOnLoad_ = false;
                initWarning_ = migrateWarn;
                initQuarantineFileName_.clear();
                warnOut = migrateWarn;
                return true;
            }
        }
        // 到这里：config.json 存在，但不可读 或 解析/schema 校验被拒 ⇒ 无法装载。
        // 不变量（**仅限本次 initialize**）：登记为 rejected 时，config.json 要么改名另存、要么原封
        // 不动——隔离失败就跳过落盘，绝不就地覆盖它。⚠️ 该不变量**不延伸到进程后续生命周期**：
        // 此后的 PUT（updateFromJsonBody）与热重载（reloadFromDisk 的 .bak 回滚）都会按各自的既有
        // 语义重写 config.json（PUT 会先备份到 .bak）。故隔离失败且服务继续运行时，那份坏配置
        // **可能被后续写入替换**——「保全」只保证到本次启动结束。
        loadRejected = true;
        warnOut = err;  // 保留原有原因原文（IO 错误 / 解析或 schema 错误）

        std::filesystem::path movedTo;
        std::string qerr;
        if (quarantineConfigFile(configPath_, movedTo, qerr)) {
            quarantineName = movedTo.filename().string();
            warnOut += "; 原配置已隔离为 " + quarantineName;
        } else {
            // 隔离失败 ⇒ 原坏文件仍在，绝不能落盘默认值（那会覆盖销毁它）
            warnOut += "; 隔离失败（" + qerr + "），已保留原文件、本次不落盘默认值";
        }
    }

    // 独立可见通道：stderr 是 initialize 阶段**唯一**不依赖任何配置来源的输出通道——它只写 fd 2，
    // 既不读配置，也不需要 EventLogger/StructuredLogger 已打开（那两者此刻尚未初始化，其目录还要由
    // 随后生效的配置决定）。此处刻意不用 rklog：本服务（src/win/）**根本未接入 rklog**。
    // 仅装载失败时输出，正常启动零输出（不污染既有测试输出）。
    if (loadRejected) {
        std::cerr << "[rk_wcfr] 配置装载失败，本次以默认值降级运行: " << warnOut << std::endl;
    }

    // 迁移：从旧 INI 读取（兼容已有流程），然后落盘为 JSON
    AppConfig cfg = loadConfigFromIniOrDefault();
    cfg.configPath = configPath_;
    const std::string pretty = buildSettingsJson(cfg, false, true);
    // 落盘前置闸门：序列化产物必须自洽（能过 settings schema）。密钥不可用时 buildSettingsJson
    // 返回错误 stub，写盘会让「下次启动必被 schema 拒 ⇒ 再隔离、再写同一 stub」自持续循环。
    std::string notWritable;
    const bool serializable = serializedDocIsWritable(pretty, notWritable);
    // 隔离失败（loadRejected 且未取得隔离文件名）⇒ config.json 仍是那份坏配置，跳过写盘以免覆盖销毁。
    const bool skipWrite = !serializable || (loadRejected && quarantineName.empty());
    if (!skipWrite) {
        std::string writeErr;
        bool writeOk = false;
        // ★ 承重不变量（FACE-CONFIG-013）：任何**写入 config.json 内容**的路径都必须持 mu_（见类级不变量）。本处已持。
        //   同迁移分支：initialize 处于启动阶段、无并发消费者，持锁为**幂等加固**（统一约束形态）。
        {
            std::lock_guard<std::mutex> lock(mu_);
            writeOk = writeAtomicallyWithBackup(pretty, writeErr);
        }
        if (!writeOk) {
            warnOut = warnOut.empty() ? ("写入默认 JSON 配置失败: " + writeErr) : (warnOut + "; " + writeErr);
            // 即使落盘失败，也允许继续用内存配置运行
        }
    } else if (!serializable) {
        // 失败要有可见性：把「为何拒写」并入 warnOut（经 initWarning 暴露为 health.configWarning），
        // 使「密钥不可用」这一状况在诊断上可区分于其它告警。
        const std::string reason =
            "默认配置序列化未通过 schema（密钥不可用或加密失败），本次不落盘: " + notWritable;
        warnOut = warnOut.empty() ? reason : (warnOut + "; " + reason);
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_ = cfg;
        lastGoodJsonPretty_ = pretty;
        std::error_code ec;
        lastWriteTime_ = std::filesystem::exists(configPath_) ? std::filesystem::last_write_time(configPath_, ec) : std::filesystem::file_time_type{};
        configRejectedOnLoad_ = loadRejected;
        initWarning_ = warnOut;
        initQuarantineFileName_ = quarantineName;
    }
    return true;
}

std::string WinJsonConfigStore::initWarning() const {
    std::lock_guard<std::mutex> lock(mu_);
    return initWarning_;
}

bool WinJsonConfigStore::configRejectedOnLoad() const {
    std::lock_guard<std::mutex> lock(mu_);
    return configRejectedOnLoad_;
}

std::string WinJsonConfigStore::initQuarantineFileName() const {
    std::lock_guard<std::mutex> lock(mu_);
    return initQuarantineFileName_;
}

AppConfig WinJsonConfigStore::current() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_;
}

uint64_t WinJsonConfigStore::generation() const {
    return generation_.load();
}

std::string WinJsonConfigStore::currentRedactedJsonPretty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return buildSettingsJson(cfg_, true, false);
}

WinJsonConfigStore::UpdateResult WinJsonConfigStore::updateFromJsonBody(const std::string& bodyUtf8) {
    UpdateResult r;
    r.httpStatus = 400;
    r.code = "invalid_request";

    JsonValue patch;
    std::string perr;
    if (!parseJson(bodyUtf8, patch, perr)) {
        r.message = "JSON 解析失败";
        r.details.push_back(perr);
        return r;
    }
    if (!patch.isObject()) {
        r.message = "settings PUT 仅接受 JSON object";
        return r;
    }

    // 基于已知落盘的 JSON 深度合并 patch，再校验+落盘。
    // ⚠️ 该基底**含 env 覆盖值**——**不是**「解耦环境覆盖项」的纯净值（旧措辞已判定为假，就地订正）：
    //   lastGoodJsonPretty_ 由**已套用** RK_WCFR_* 覆盖（parseAndValidateSettingsDoc 末尾）的 cfg 序列化而来，
    //   故一次 PUT 会把**当时的 env 值**顺带写回 config.json。属既有性质、非本批引入，详见
    //   parseAndValidateSettingsDoc 末尾的 FACE-CONFIG-015 登记块与 docs/guides/windows-service/config.md。
    std::string lastJson;
    {
        std::lock_guard<std::mutex> lock(mu_);
        lastJson = lastGoodJsonPretty_;
    }

    JsonValue baseDoc;
    std::string err;
    if (!parseJson(lastJson, baseDoc, err)) {
        r.httpStatus = 500;
        r.code = "internal_error";
        r.message = "解析落盘配置失败";
        r.details.push_back(err);
        return r;
    }

    // 脱敏全量回写保护：剔除 patch 中的 "***" 哨兵（语义=不修改，保留 base 既有值）。
    // 否则 GET 脱敏输出被原样回写时，真实密文会被字面量 "***" 覆盖并重加密销毁。
    stripMaskedSentinels(patch);

    // FACE-CONFIG-018（S-1）：写型路径字段的**真正强制**只在此处——PUT 是唯一能安全拒绝的路径
    //   （启动期拒绝会触发 INI 回落清空用户配置，见 WinConfig.h 的「只钳不拒」承重理由）。
    // 为什么只查 patch 里**显式携带**的字段（而非合并后的 baseDoc）：否则既有 config.json 里已存在
    //   `..` 的用户会「任何 PUT 都被 400」——哪怕只改摄像头宽度——被锁死在无法保存任何设置的状态
    //   （不是数据丢失，但同样是拒绝服务）。只查 patch ⇒ 拒绝面恰好等于「用户本次真的写了这个值」。
    // 为什么只覆盖两个**写型**字段：读型字段（cascadePath 等）本批只做启动期告警（分阶段强度）。
    // 判据与启动期**完全相同**（checkConfigPath 的 `..` 段判定）——两者的唯一差别是处置（拒 vs 告警）。
    // ⚠️ 绝对路径**不**在此被拒（官方用法，见 checkConfigPath 的逃生口说明）。
    {
        struct WriteTypePath {
            const char* section;
            const char* key;
        };
        static const WriteTypePath kWriteTypePaths[] = {{"recognition", "databasePath"}, {"log", "logDir"}};
        for (const WriteTypePath& wt : kWriteTypePaths) {
            const JsonValue* sec = patch.find(wt.section);
            if (!sec || !sec->isObject()) continue;
            const JsonValue* v = sec->find(wt.key);
            if (!v || !v->isString()) continue;  // 非字符串由 schema 校验负责（保持既有错误文案不变）
            std::string why;
            if (!checkConfigPath(std::filesystem::path(v->s), PathPolicy::Strict, why)) {
                r.message = "settings 未应用：写型路径字段不合法";
                r.details = {std::string(wt.section) + "." + wt.key + "：" + why};
                return r;  // r.httpStatus/r.code 已在函数入口初始化为 400 / invalid_request
            }
        }
    }

    deepMergeObject(baseDoc, patch);

    // 校验 schema
    std::string schemaErr;
    if (!JsonSchemaValidator::validate(JsonSchemaValidator::buildSettingsDocSchema(), baseDoc, &schemaErr)) {
        r.message = "Schema 校验失败";
        r.details = {schemaErr};
        return r;
    }
    // poster.postUrl：若为 object，必须满足 encrypted-object 结构
    if (const JsonValue* poster = baseDoc.find("poster"); poster && poster->isObject()) {
        if (const JsonValue* pu = poster->find("postUrl"); pu && pu->isObject()) {
            std::string encErr;
            if (!JsonSchemaValidator::validate(JsonSchemaValidator::buildEncryptedStringSchema(), *pu, &encErr)) {
                r.message = "/poster/postUrl 加密对象校验失败";
                r.details = {encErr};
                return r;
            }
            if (const JsonValue* alg = pu->find("alg"); alg && alg->isString()) {
                if (alg->s != "AES-256-GCM") {
                    r.message = "/poster/postUrl/alg: 仅支持 AES-256-GCM";
                    r.details = {alg->s};
                    return r;
                }
            }
        }
    }

    // 将 doc 序列化并解析为 AppConfig（会解密密文字段）
    const std::string mergedText = toJsonString(baseDoc, true);
    AppConfig newCfg;
    if (!parseAndValidateSettingsDoc(mergedText, newCfg, err)) {
        r.message = "settings 应用失败";
        r.details.push_back(err);
        return r;
    }
    // FACE-ARCWIN-010 / FACE-CONFIG-017 / FACE-CONFIG-018 的规范化告警：**本路径无对外可见通道**
    //   （HTTP 层只在**错误**响应里带 details，见 JsonEndpointHandlers 的 errJson；改它超出本批允许
    //   改动的文件范围）⇒ 此处只**取走**，避免它残留到下一次读取被误当成那一次的告警。
    //   「钳制结果本身可见（GET settings 返回钳后的生效值）」这句话**只对 log 段成立**，勿推广
    //   （分字段说明，逐条核实过；原注释一概而论，已订正）——判据用**函数名**而非行号，避免漂移：
    //   · log.maxFileBytes / log.maxRollFiles：PUT 路径的 schema 对 log 段上下界照旧宽松（1 GiB /
    //     1000）⇒ 原值通过本函数内那句 `JsonSchemaValidator::validate(...)`（在 deepMergeObject 之后、
    //     parseAndValidateSettingsDoc 之前），钳制发生在随后那句
    //     `parseAndValidateSettingsDoc(mergedText, newCfg, err)` 内 ⇒ 提交的 cfg_ 是**钳后**值，
    //     GET settings 可见；但**本次 PUT 的响应**不含告警文本（通道缺口如上，告警被 (void) 丢弃）。
    //   · recognition.identifyThreshold：PUT 上**不是「钳」而是「拒」**——那句 schema 校验发生在
    //     parseAndValidateSettingsDoc **之前**，且该键的 maximum 已收紧到 64 ⇒ `> 64` 的 PUT 在 schema
    //     处就被 **400 拒绝**，normalizeRecognitionKeys 里的钳制分支
    //     （判据 `> kIdentifyThresholdMax` 才触发）在 PUT 路径**不可达**；该钳制只在启动 / 热重载
    //     的 JSON 与 INI 读入路径生效（启动期钳制**在前**是刻意的，见其定义处）。
    //   详见 g_normalizeWarnings 处的通道去向上限登记。
    (void)takeNormalizeWarnings();

    // 落盘：敏感字段强制加密存储。
    // ★ 承重不变量（FACE-CONFIG-013）：任何**写入 config.json 内容**的路径都必须持 mu_（见类级不变量）。本处已持。
    // ★ 并发互斥（FACE-CONFIG-010 竞态收口）：写盘与内存提交（cfg_/generation_）必须在**同一** mu_ 临界区内，
    //   以便与 reloadFromDisk 回滚段的「取快照 → 序列化 → 写回磁盘」互斥，杜绝「回滚线程用旧快照覆盖已提交的
    //   新 PUT」这一竞态（详见 reloadFromDisk 回滚处的同因注释）。
    // ★ 密钥/密文同源不变量（项 A）：**加密也必须在本临界区内**——buildSettingsJson 经 ensureOrLoadKey
    //   **每次重读密钥文件**；若加密在锁外，并发的 rotateKeyAndReencrypt 可能在「读密钥」与「写盘」之间
    //   把密钥文件换成新密钥 ⇒ 本次密文用旧密钥、盘上密钥已是新密钥（或反之）⇒ 下次启动解密失败。
    //   故把「加密 + 写盘 + 提交」整段纳入同一 mu_ 临界区。
    //   为何可直接扩大 mu_（而非新增专用写锁）：buildSettingsJson / writeAtomicallyWithBackup **均不取 mu_**
    //   （逐字核实），故持 mu_ 调用它们不会递归加锁；全函数仅此一把锁 ⇒ 无锁序问题、无死锁风险。
    //   代价（如实登记）：加密（DPAPI 解保护 + AES-256-GCM）纳入持锁时长 ⇒ 加密期间 mu_ 被持有，
    //   current() / 热重载会被阻塞 ≈ 一次加密耗时（PUT 为低频管理操作，可接受；reloadFromDisk 回滚段早已如此）。
    {
        std::lock_guard<std::mutex> lock(mu_);
        const std::string prettyEnc = buildSettingsJson(newCfg, false, true);
        if (prettyEnc.find("\"_error\"") != std::string::npos) {
            r.httpStatus = 500;
            r.code = "crypto_error";
            r.message = "敏感字段加密失败";
            r.details.push_back(prettyEnc);
            return r;
        }
        if (!writeAtomicallyWithBackup(prettyEnc, err)) {
            r.httpStatus = 500;
            r.code = "io_error";
            r.message = "写入配置失败";
            r.details.push_back(err);
            return r;
        }
        cfg_ = newCfg;
        lastGoodJsonPretty_ = prettyEnc;
        std::error_code ec;
        lastWriteTime_ = std::filesystem::last_write_time(configPath_, ec);
        generation_.fetch_add(1);  // PUT 成功提交：配置代数 +1（消费方据此恰好消费一次）
    }

    r.ok = true;
    r.httpStatus = 200;
    r.code = "ok";
    r.message = "OK";
    return r;
}

WinJsonConfigStore::UpdateResult WinJsonConfigStore::rotateKeyAndReencrypt() {
    UpdateResult r;
    r.httpStatus = 500;
    r.code = "internal_error";

    // ★ 不变量（承重）：**盘上的 config.json 必须始终能用盘上的密钥文件解开**（同源）。
    //   实现口径（项 A）：密钥文件与 config.json 内容**只在同一个 mu_ 临界区内、以同一密钥成对写入**
    //   ——「写新密钥 → 用它加密 → 写 config.json → 提交」整段持锁 ⇒ 其它写盘路径（PUT / 回滚）
    //   不可能观察到「新密钥 + 旧密文」或「旧密钥 + 新密文」的中间态。
    //   ⚠️ 修复前的失效模式：`writeFileAll(keyPath_, ...)` 写在 mu_ **之外**，而 PUT 的加密
    //      （buildSettingsJson → ensureOrLoadKey **每次重读密钥文件**）也在 mu_ 外 ⇒ 某 PUT 可在
    //      rotate 写入新密钥**之后**读取密钥并加密 config.json，随后 rotate 的代次复核发现变化 ⇒
    //      冲突分支 restoreOldKey() 把**旧**密钥写回 ⇒ 磁盘密钥=旧、磁盘密文=新 ⇒ 下次启动解密失败。
    //   修法：① 冲突分支**不触碰任何磁盘状态**（新密钥仅存在于内存，直接丢弃，**不再** restore）；
    //        ② 密钥文件与 config.json 在同一临界区内、以同一密钥成对写入；③ PUT 的加密也移入其写盘临界区。
    //   ⚠️ 并发互斥（FACE-CONFIG-010 竞态收口）：写盘 + 提交（cfg_）同处一个 mu_ 临界区，并在同一
    //      临界区内复核 generation_ 未变 ⇒ 并发 PUT / 热重载提交的新配置不会被 rotate 的旧快照覆盖。
    //      修法选**代次校验 + 有界重试**：锁内记 generation_，提交前在同一临界区复核；变了则**不写盘**、
    //      重取快照重试（有界），耗尽返回 409 conflict。generation_ 是可靠信号：PUT 成功与热重载成功
    //      都会 fetch_add(1)；rotate 自身不变代数。
    //   ⚠️ 代价（如实登记）：提交临界区含「写密钥文件 + AES 加密 + 写 config.json」；故持锁时长 ≈
    //      一次写密钥 + 一次加密 + 一次原子写（比「整段持 mu_」短——随机数生成与 DPAPI 保护仍在锁外）。
    //   ⚠️ 残余（如实登记）：密钥文件与 config.json 是**两个文件**，无法原子同写 ⇒ 「写密钥之后、
    //      写 config.json 之前」若**进程崩溃**，磁盘仍可能错配（下次启动隔离）。本批只消除**并发交错**
    //      造成的错配，**不消除**该崩溃窗口。
    constexpr int kMaxRotateAttempts = 3;
    std::string conflictMsg;
    for (int attempt = 0; attempt < kMaxRotateAttempts; ++attempt) {
        AppConfig cfg;
        std::uint64_t genAtSnapshot = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cfg = cfg_;
            genAtSnapshot = generation_.load();
        }

#if defined(RK_ENABLE_TEST_SEAMS)
        // ★ 测试接缝（编译期开关，仅 win_unit_tests 定义；见头文件 setRotateTestHookForTesting 声明注释）：
        //   在「取快照之后、生成并替换密钥之前」同步触发一次由测试注入的动作（典型：合法 PUT）。此刻
        //   **未持 mu_** ⇒ 钩子内调 updateFromJsonBody 不会自锁。**必须在本函数替换密钥文件之前**触发：
        //   否则磁盘上的 postUrl 密文仍是旧密钥加密的，PUT 的解析/解密会失败（那不是本用例要构造的交错）。
        //   语义/数据不变；本块新增一次 mu_ 获取/释放（安全读钩子用）。默认未设置（空 std::function）⇒ 恒假。
        {
            std::function<void()> testHook;
            {
                std::lock_guard<std::mutex> lock(mu_);
                testHook = rotateTestHook_;
            }
            if (testHook) testHook();
        }
#endif

        // 生成并 DPAPI 保护新密钥：**纯计算 + DPAPI，均在锁外**，且**不触碰磁盘**——密钥文件留到
        // 提交临界区内才写（这是「密钥/密文同源」的关键：冲突时磁盘密钥从不被这次调用改动）。
        std::vector<std::uint8_t> newKey;
        std::string kerr;
        if (!randomBytes(32, newKey, kerr)) {
            r.code = "crypto_error";
            r.message = "生成新密钥失败";
            r.details.push_back(kerr);
            return r;
        }
        std::vector<std::uint8_t> protectedBin;
        if (!dpapiProtect(newKey, protectedBin, kerr)) {
            r.code = "crypto_error";
            r.message = "DPAPI 保护密钥失败";
            r.details.push_back(kerr);
            return r;
        }
        std::string newKeyB64;
        if (!base64Encode(protectedBin, newKeyB64)) {
            r.code = "crypto_error";
            r.message = "密钥 base64 编码失败";
            return r;
        }
        std::string ioErr;
        if (!ensureParentDir(keyPath_, ioErr)) {
            r.code = "io_error";
            r.message = "创建密钥目录失败";
            r.details.push_back(ioErr);
            return r;
        }

        std::string err;
        bool retry = false;
        // ★ 承重不变量（FACE-CONFIG-013）：任何**写入 config.json 内容**的路径都必须持 mu_（见类级不变量）。
        // ★ 密钥/密文同源（项 A）：密钥文件与 config.json 内容在**同一临界区**内成对写入。
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (generation_.load() != genAtSnapshot) {
                // 快照已过期：期间 PUT / 热重载提交了新配置。**不触碰任何磁盘状态**（新密钥仅存在于
                // 内存，直接丢弃）⇒ 磁盘密钥/密文同源不变量保持。重取快照重试（有界）。
                conflictMsg = "配置在密钥轮换期间被并发修改（PUT / 热重载），已重取快照重试";
                retry = true;
            } else {
                // 读当前磁盘密钥快照，仅供「已写新密钥、提交前失败」的**回滚**（两文件必须同源）。
                const bool hadOldKey = std::filesystem::exists(keyPath_);
                std::string oldKeyB64;
                if (hadOldKey && !readFileAll(keyPath_, oldKeyB64, ioErr)) {
                    r.code = "io_error";
                    r.message = "读取旧密钥失败";
                    r.details.push_back(ioErr);
                    return r;
                }
                const auto restoreOldKey = [&]() {
                    if (hadOldKey) {
                        std::string e;
                        (void)writeFileAll(keyPath_, oldKeyB64, e);
                    } else {
                        std::error_code ec;
                        std::filesystem::remove(keyPath_, ec);
                    }
                };

                // ★ 成对写入：写新密钥 → 用新密钥加密（ensureOrLoadKey 读到的即刚写入的新密钥）
                //   → 写 config.json → 提交。整段持锁，故外部看不到「新密钥 + 旧密文」的中间态。
                if (!writeFileAll(keyPath_, newKeyB64, ioErr)) {
                    r.code = "io_error";
                    r.message = "写入新密钥失败";
                    r.details.push_back(ioErr);
                    return r;
                }
                const std::string prettyEnc = buildSettingsJson(cfg, false, true);
                if (prettyEnc.find("\"_error\"") != std::string::npos) {
                    restoreOldKey();
                    r.code = "crypto_error";
                    r.message = "使用新密钥加密失败";
                    r.details.push_back(prettyEnc);
                    return r;
                }
                if (!writeAtomicallyWithBackup(prettyEnc, err)) {
                    restoreOldKey();
                    r.code = "io_error";
                    r.message = "写入配置失败（已尝试回滚密钥）";
                    r.details.push_back(err);
                    return r;
                }
                cfg_ = cfg;
                lastGoodJsonPretty_ = prettyEnc;
                std::error_code ec;
                lastWriteTime_ = std::filesystem::last_write_time(configPath_, ec);
            }
        }
        if (retry) continue;

        r.ok = true;
        r.httpStatus = 200;
        r.code = "ok";
        r.message = "OK";
        return r;
    }

    // 有界重试耗尽：配置在 rotate 期间被**持续**并发修改 ⇒ 放弃本次轮换并如实上报（不静默用旧快照覆盖）。
    // 不变量仍成立：本函数从未在冲突分支写盘 ⇒ 磁盘上的（密钥，密文）对仍是上一次成功写入时的成对状态。
    r.httpStatus = 409;
    r.code = "conflict";
    r.message = conflictMsg.empty() ? "配置在密钥轮换期间被并发修改，已放弃本次轮换" : conflictMsg;
    return r;
}

bool WinJsonConfigStore::reloadFromDisk(bool& outApplied, std::string& outErr) {
    outErr.clear();
    outApplied = false;

    std::error_code ec;
    if (!std::filesystem::exists(configPath_)) return true;

    std::string txt;
    std::filesystem::file_time_type wt;
    {
        std::lock_guard<std::mutex> lock(mu_);
        wt = std::filesystem::last_write_time(configPath_, ec);
        if (ec) return true;
        if (lastWriteTime_ == wt) return true;
        if (!readFileAll(configPath_, txt, outErr)) {
            // ★ **2026-09-28 改**（外部审查 [P2]「一次临时读取失败会使热重载停止重试」+ 独立复核）：
            //   原行为（`2cf7a0e` 有意引入、并在此处写明理由）记录**已观察到的 mtime**，以避免
            //   「文件持续不可读时每秒反复重读并失败」。**其副作用**才是真问题：读取失败后
            //   `lastWriteTime_ == wt` 恒成立 ⇒ 后续每次轮询都在上一行 `lastWriteTime_ == wt` 处早退，
            //   直到 mtime 变化。而 Windows 上**解除占用 / 放宽 ACL 都不会更新 LastWriteTime**
            //   （只更新 ChangeTime）⇒ 文件恢复可读后，配置会一直陈旧到**下一次真实写入或重启**，
            //   期间没有任何用户可见提示。
            //   ⇒ 改为**不标记**：读取失败不算「已处理」，下次轮询（≤1s）重试。
            //   **代价（如实登记）**：文件持续不可读期间，每秒一次**失败的 `open()`**。
            //   ⚠️ 且该 `open()` 发生在**上面那个 mu_ 临界区内**（对抗性代理 A2 指出，初版未登记这一点）：
            //      若该文件位于慢设备/网络盘上、失败的 open 本身也阻塞，则 PUT 与 `current()` 会被
            //      **周期性**拖住。改前是「首次失败即标记 ⇒ 之后早退」，**不存在**这种周期性重入
            //      ⇒ 这是本次修复**新引入**的代价，不是既有行为。
            //   该分支不做解析、不打日志 ⇒ 噪声仅为 IO；以「每秒一次失败 open」换「恢复后 ≤1s 自动
            //   重载」，判为更优。（若将来出现病态 IO 噪声，再引入**有界退避**：前 N 次不标记、之后
            //   退化为旧行为——既保原意图又不牺牲最终一致。本轮**不做**，避免过度设计。）
            return false;
        }
    }

    AppConfig cfg;
    std::string parseErr;
    if (!parseAndValidateSettingsDoc(txt, cfg, parseErr)) {
        // 热重载校验失败：与启动路径（initialize）对齐的**证据保全**——先把坏文件改名另存
        // （config.json.corrupt.<毫秒时间戳>），使其不被随后写盘覆盖。
        // ⚠️ 该保全**仅限本次处理**（不变量有同样时限）：隔离产物不会被自动清理，但原位置的后续写入
        //    （PUT / 下一次热重载）仍按各自既有语义进行，故「不被就地覆盖」只保证到本次处理结束。
        // ★ **2026-09-28 已修**（外部审查 [P2]「热重载可能隔离刚保存的正常配置」+ 独立复核确认可达）：
        //    原行为是「解析在锁外、隔离也在锁外」，由此存在一条**可达竞态**：
        //      ① 本函数开头的读锁临界区已**释放 mu_**；② watch 线程在锁外解析 txt 被判坏；
        //      ③ 此窗口内 HTTP 线程 PUT 成功，写盘合法配置 B 并提交 cfg_=B；
        //      ④ 本线程随即执行 quarantineConfigFile，把**这份刚写好的合法 B** 改名成
        //         config.json.corrupt.<ts> ⇒ 凭空伪造一条 corrupt 证据（且下述 outErr 谎称隔离了
        //         「坏配置」），该证据永不自清；
        //      ⑤ 再持锁把 cfg_(=B) 写回——在「内存序列化不可写 ⇒ 回退 .bak」那条臂里，磁盘会退成
        //         **更旧的 .bak** 而内存是 B ⇒ 正是 FACE-CONFIG-010 刚消除的「磁盘/内存分歧」。
        //    **修法**：把「复核 mtime 未变」与「改名」放进**同一个 mu_ 临界区** —— 与下方成功臂
        //      （`if (!cec && currentWt != wt) { …放弃本次更新 }`）**完全同款**。PUT 的写盘 + 提交
        //      持的是同一把 mu_ ⇒ 复核与 rename 之间不存在 **PUT** 插入点，**针对进程内写者的**竞态闭合。
        //    ⚠️ 措辞收窄（对抗性代理 A2 指出）：上面那个「闭合」**只对进程内写者成立**。对**外部**
        //      写者，判据仍是 mtime 比对，而文件系统时间戳有粒度（NTFS/卷粒度可粗至 ~15.6ms 或更粗）
        //      ⇒ 坏文件与合法 B 落在**同一 tick** 时二者 mtime 相同、复核漏判。这是 mtime 判据的
        //      **固有盲区**（成功臂 `currentWt != wt` 处同款，非本批引入），不要据此认为「任何并发修改都能识别」。
        //    **行为变化（如实登记）**：① 在「读后到隔离前」被并发修改的交错下**不再隔离** —— 此时该
        //      路径上已不是坏文件，隔离它本就是错的；② 若存在**持续**触碰 config.json 的外部写者，
        //      坏文件的隔离会被反复推迟 —— 与成功臂「放弃本次热重载」的既有语义一致。
        //    ⚠️ 仍**不在** FACE-CONFIG-013「写入内容须持 mu_」的射程内（改名不产生新内容），本处加锁
        //      是为**身份复核**，不是为了满足那条不变量——两件事的理由不同，勿混。
        outErr = "热重载校验失败: " + parseErr;
#if defined(RK_ENABLE_TEST_SEAMS)
        // ★ 测试接缝（编译期开关，仅 win_unit_tests 定义；见头文件 setReloadTestHookForTesting 声明注释）：
        //   在「解析被拒之后、重新取锁复核 mtime 之前」同步触发一次由测试注入的动作（典型：合法 PUT）。
        //   此刻**未持 mu_** ⇒ 钩子内调 updateFromJsonBody 不会自锁。默认未设置（空 std::function）⇒ 恒假。
        //   语义/数据不变；本块新增一次 mu_ 获取/释放（安全读钩子用），读取在 mu_ 下、调用在锁外，
        //   避免与 setter 的数据竞争。
        {
            std::function<void()> testHook;
            {
                std::lock_guard<std::mutex> lock(mu_);
                testHook = reloadTestHook_;
            }
            if (testHook) testHook();
        }
#endif
        std::filesystem::path movedTo;
        std::string qerr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::error_code cec;
            const auto curWt = std::filesystem::last_write_time(configPath_, cec);
            if (cec || curWt != wt) {
                outErr += "; 配置在本次读取期间已被并发修改 ⇒ 放弃隔离（避免把新内容当作坏文件改名）";
                return false;
            }
            if (!quarantineConfigFile(configPath_, movedTo, qerr)) {
                // 隔离失败 ⇒ 坏文件仍在原位。记录**已观察到的 mtime**，否则 1 秒轮询会反复重读同一份
                // 坏文件并反复失败（IO / 日志噪声）。
                // ⚠️ 已知同类残留（**本轮未改**，如实登记）：与上面 `readFileAll` 失败臂同族 —— 若此后
                //    该文件的可改名条件恢复而 mtime 未变，本路径同样不会再重试。**不改的理由**：此处的
                //    重试成本是**完整解析 + DPAPI 解保护**（而非一次 open），且该文件**已知为坏**，
                //    「配置陈旧」并非本臂的失效形态；用户的修复动作（改写该文件）本身会改 mtime ⇒ 会
                //    重新进入本函数。
                outErr += "; 隔离失败（" + qerr + "），保留原文件";
                std::error_code wec;
                lastWriteTime_ = std::filesystem::last_write_time(configPath_, wec);
                return false;
            }
        }
        outErr += "; 原配置已隔离为 " + movedTo.filename().string();
        // 隔离已把坏文件挪走；随后把**内存中当前生效的配置**写回 config.json，使磁盘与内存**语义一致（非字节一致）**。
        // FACE-CONFIG-010：为什么不用 .bak —— .bak 是「上一次写入前」的副本，可能比内存中正在生效的
        //   配置**更旧**（例：用户刚 PUT 过 B，.bak 仍是 A）。把它写回磁盘会得到「磁盘 = 旧 A、内存 = 新 B」
        //   的不一致，静默回退用户最近一次成功 PUT 的持久化形态，直到下一次真实变更或重启才收敛。
        //   故首选内存快照；仅当内存序列化不可写时才回退 .bak（保持服务可用），两者都不可用则不写盘。
        // 为什么用 cfg_ 重新序列化而非缓存的 lastGoodJsonPretty_ —— lastGoodJsonPretty_ **有意可能滞后**
        //   于 cfg_（成功热重载路径在序列化为错误 stub 时**不**更新它，见下方成功分支），故它不总等于当前
        //   生效配置；cfg_ 才是「当前生效配置」的事实源（与 configRejectedOnLoad 语义同源）。
        // 落盘前必须过 serializedDocIsWritable 闸门：密钥不可用时 buildSettingsJson 返回 {"_error":...}
        //   stub，写盘会污染磁盘（下次启动必被 schema 拒 ⇒ 再隔离 ⇒ 自持续循环）。
        // 写回方式为「直接写 tmp + moveReplaceWriteThrough」，**不**走 writeAtomicallyWithBackup——
        //   后者会轮转 .bak（把 config.json.bak 改名为 .bak.1），搅乱本轮「隔离证据 → 回滚」的证据链。
        // **不采纳**任何回滚内容进内存（不 cfg_ = ...、不 generation+1）：避免一次无关的外部坏写撤销用户
        //   最近一次成功 PUT 并经 generation 触发整条管线 FullRestart——这是本次修复要保住的语义。
        // ★ 承重不变量（FACE-CONFIG-013）：任何**写入 config.json 内容**的路径都必须持 mu_（见类级不变量）。本处已持。
        // ★ 并发互斥（FACE-CONFIG-010 竞态收口；本文件所有写盘调用点统一用 mu_ 串行化）
        // 以下「取内存快照 → 序列化 → 写回磁盘 → 记录 mtime」**整段**必须在 mu_ 临界区内完成，与
        // updateFromJsonBody（PUT）及 rotateKeyAndReencrypt 的「写盘 + 内存提交」互斥。
        // 否则失效时序（三路对抗审查逐字推演，已复核为真实可达）：回滚线程取到旧快照 A → 并发 PUT 成功
        //   写盘 B 且提交 cfg_=B、generation++ → 回滚线程继续把 A 写回 config.json（**覆盖 B**）→ 再把
        //   lastWriteTime_ 记为 A 的 mtime ⇒ 下一轮轮询因 mtime 相同早退 ⇒ 磁盘=A、内存=B 的分歧持久不
        //   收敛，重启读到的就是被撤销的 A。这与 FACE-CONFIG-010 声称消除的失败是**同一个**，只是从确定性
        //   版变成竞态版。
        // 为何直接用 mu_ 而非新增专用写锁：mu_ 已是内存态的唯一锁；本段调用的 buildSettingsJson /
        //   serializedDocIsWritable / writeFileAll / moveReplaceWriteThrough / parseAndValidateSettingsDoc
        //   均**不取 mu_**（逐字核实）⇒ 不会递归加锁；所有写盘调用点都只用 mu_ 一把锁 ⇒ 无锁序问题、无死锁。
        //   代价：写盘期间 mu_ 被持有 ⇒ current() 短暂阻塞（本路径只在检测到外部坏写时触发，罕见）。
        //   ⚠️ 代价不止「一次文件写」：本临界区还含 **AES-256-GCM 加密**（buildSettingsJson 对 poster.postUrl
        //     重加密）、**DPAPI 解保护**（ensureOrLoadKey）与**多次文件 I/O**（写 tmp + 原子替换；.bak 臂还要
        //     先读 .bak），故读侧（任何需 cfg_ 的调用，如 current()）最坏阻塞 ≈ **一次完整回滚**，而非单次 write。
        // rbTmp 为**固定名** `config.json.rollback.tmp`：写入 + moveReplaceWriteThrough 成功即消失；但进程
        //   若在写盘中途崩溃**会残留**该文件。FACE-CONFIG-014 已修：initialize 启动时会清扫该残留（见其内
        //   注释与 WinJsonConfig.h），故残留最多存活到下次启动，无需人工处理。残留无正确性影响（下次回滚以
        //   trunc 覆盖），仅占一份磁盘空间。
        const std::filesystem::path rbTmp = configPath_.parent_path() / L"config.json.rollback.tmp";
        {
            std::lock_guard<std::mutex> lock(mu_);
            const std::string memPretty = buildSettingsJson(cfg_, false, true);
            std::string memWhyNot;
            if (serializedDocIsWritable(memPretty, memWhyNot)) {
                std::string ioErr;
                if (writeFileAll(rbTmp, memPretty, ioErr) && moveReplaceWriteThrough(rbTmp, configPath_, ioErr)) {
                    outErr += "; 已用内存中当前生效的配置写回 config.json（本次不应用）";
                } else {
                    std::error_code rec;
                    std::filesystem::remove(rbTmp, rec);
                    outErr += "; 内存配置写回失败: " + ioErr;
                }
            } else {
                // ② 订正：进入本臂的**现实**原因**不是**「密钥不可用」。.bak 臂先经 parseAndValidateSettingsDoc，
                //   而该函数**无条件**先调 ensureOrLoadKey ⇒ 密钥不可用时 .bak 会被同判「也无效」⇒ 落到下面
                //   「无可用 .bak」臂。故「密钥不可用」下**两臂皆不可用 ⇒ 不写盘**。本臂真正可达的场景是
                //   「密钥可用但加密失败」——即 buildSettingsJson 因 aes256gcmEncrypt 失败返回
                //   {"_error":"encrypt_error"} stub，此时 .bak 用同一把**可用**密钥仍能解析成功 ⇒ 才真正回退
                //   .bak。属极罕见分支。
                outErr += "; 内存配置不可写（" + memWhyNot + "），回退 .bak";
                if (std::filesystem::exists(bakPath_)) {
                    std::string bakTxt, e2;
                    if (readFileAll(bakPath_, bakTxt, e2)) {
                        AppConfig bakCfg;
                        if (parseAndValidateSettingsDoc(bakTxt, bakCfg, e2)) {
                            std::string ioErr;
                            if (writeFileAll(rbTmp, bakTxt, ioErr) && moveReplaceWriteThrough(rbTmp, configPath_, ioErr)) {
                                outErr += "; 已用 .bak 写回 config.json（本次不应用）";
                            } else {
                                std::error_code rec;
                                std::filesystem::remove(rbTmp, rec);
                                outErr += "; 回滚写入失败: " + ioErr;
                            }
                        } else {
                            outErr += "; .bak 也无效: " + e2;
                        }
                    } else {
                        outErr += "; 读取 .bak 失败: " + e2;
                    }
                } else {
                    // ③ 后果（**可达**：密钥不可用时两臂皆不可用，正是走这里）：坏文件已被隔离 ⇒ config.json
                    //   **不存在**。下次启动进入「不存在」分支 ⇒ loadRejected=false ⇒ 以默认值运行、configStatus
                    //   报 ok（密钥也坏时为 degraded）⇒ **用户原配置永远不会被自动恢复**——只能人工把
                    //   config.json.corrupt.<ts> 改名回 config.json。需人工介入，非自愈。
                    outErr += "; 无可用 .bak，本次不写盘; 原配置未自动恢复，需人工从 config.json.corrupt.* 改回";
                }
            }
            // 记录**实际**当前 mtime（写回内容时文件已被本次改写；隔离后若未写回则文件不存在、取默认值），
            // 打断 1 秒轮询的重复重读（否则会每秒重读一份已判定为坏 / 已被替换的文件，产生 IO / 日志噪声）。
            // ⚠️ 残余不一致（如实登记，非第三种语义）：本路径返回时内存配置保持原样（不应用）。
            //   · 首选路径（内存快照写回）下磁盘与内存**语义一致（非字节一致）**——buildSettingsJson 会重加密
            //     postUrl（新 nonce）、对 inference.* 走归一化/钳制、且**不**序列化 AppConfig.configPath，
            //     故两份 JSON 不是字节等价；这正是 FACE-CONFIG-010 要达到的目标；
            //   · 回退 .bak 路径下 .bak 可能比内存中生效的配置更旧 ⇒ 「磁盘 = 旧 .bak，内存 = 较新配置」
            //     短暂不一致，直到下一次真实变更或重启才收敛。该回退仅在「密钥可用但内存加密失败」的极罕见
            //     分支发生，是为「不应用」语义（不重启管线、不撤销用户最近一次 PUT）付出的代价。
            std::error_code cec;
            if (std::filesystem::exists(configPath_)) {
                lastWriteTime_ = std::filesystem::last_write_time(configPath_, cec);
            } else {
                lastWriteTime_ = std::filesystem::file_time_type{};
            }
        }
        return false;  // ★ 不应用：不 cfg_ = cfg、不 generation+1、outApplied 保持 false
    }

    // FACE-ARCWIN-010 / FACE-CONFIG-017 / FACE-CONFIG-018 的规范化告警（热重载路径）。
    // ⚠️ 如实登记：热重载**没有**用户可见通道——outErr 是唯一被调用方接走的字符串，而两个生产调用点
    //   （win_local_service_main / win_camera_face_recognition_main）都只读 outApplied、忽略 err，
    //   HTTP 层也没有「重载告警」出口（改那些文件超出本批允许范围）。此处并入 outErr 只为让
    //   测试 / 日志能看到「本次热重载发生了钳制或路径告警」；**用户可见通道仍只有 initialize 的
    //   warnOut ⇒ health 的 configWarning**。语义代价（已知）：成功路径上 outErr 可能非空
    //   （outApplied 仍为 true），故读取方**不得**用「outErr 非空」判定失败——既有调用点已如此。
    const std::string normWarn = takeNormalizeWarnings();
    if (!normWarn.empty()) {
        outErr = outErr.empty() ? normWarn : (outErr + "; " + normWarn);
    }

    cfg.configPath = configPath_;
    const std::string pretty = buildSettingsJson(cfg, false, true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::error_code cec;
        const auto currentWt = std::filesystem::last_write_time(configPath_, cec);
        if (!cec && currentWt != wt) {
            // 文件在读取后被再次修改，放弃本次更新以避免覆盖
            outErr = "配置在读取期间被外部修改，放弃热重载";
            return false;
        }
        cfg_ = cfg;
        // 序列化产物若为错误 stub（密钥运行期失效 ⇒ buildSettingsJson 返回 {"_error":...}），不得
        // 记为「最近良好序列化」——否则后续 PUT 会以该 stub 为 base 合并（updateFromJsonBody 对自身
        // 产物另有 "_error" 检查，此处对齐同一闸门）。
        if (pretty.find("\"_error\"") == std::string::npos) {
            lastGoodJsonPretty_ = pretty;
        }
        // 本路径未写盘，wt 仍是当前 mtime（上面已确认读取期间无外部修改），直接沿用即可；
        // 写入 lastWriteTime_ 使下一次轮询在同一 mtime 上早退（避免每秒重读同一份内容）。
        lastWriteTime_ = wt;
        generation_.fetch_add(1);  // 外部写入成功 reload：配置代数 +1
    }
    outApplied = true;
    return true;
}

bool WinJsonConfigStore::pollReloadOnce(bool& outApplied, std::string& outErr) {
    return reloadFromDisk(outApplied, outErr);
}

#if defined(RK_ENABLE_TEST_SEAMS)
// ★ 测试接缝（编译期开关，仅 win_unit_tests 定义；见头文件声明注释）。生产构建不编译本段 ⇒ 无副作用。
void WinJsonConfigStore::setReloadTestHookForTesting(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(mu_);
    reloadTestHook_ = std::move(hook);
}

void WinJsonConfigStore::setRotateTestHookForTesting(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(mu_);
    rotateTestHook_ = std::move(hook);
}
#endif

bool WinJsonConfigStore::startWatching() {
    stopWatching();
    watching_ = true;
    watchThread_ = std::thread([this]() {
        while (watching_) {
            bool applied = false;
            std::string err;
            (void)reloadFromDisk(applied, err);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    });
    return true;
}

void WinJsonConfigStore::stopWatching() {
    watching_ = false;
    if (watchThread_.joinable()) watchThread_.join();
}

}  // namespace rk_win
