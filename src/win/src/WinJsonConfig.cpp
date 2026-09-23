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
        r.o["identifyThreshold"] = JsonValue::makeNumber(cfg.recognition.identifyThreshold);
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
        l.o["maxFileBytes"] = JsonValue::makeNumber(static_cast<double>(cfg.log.maxFileBytes));
        l.o["maxRollFiles"] = JsonValue::makeNumber(cfg.log.maxRollFiles);
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

// 键级规范化（FACE-ARCWIN-006/007 迁移过渡）。定义见本文件 parseAndValidateSettingsDoc 之后。
// 前置声明置于此，仅为保持定义靠近其唯一调用点之外的既有函数布局（定义体含 doc.find 字面量，
// 放在本处会前移既有静态切片测试的识别段起点）。
static void normalizeRecognitionKeys(JsonValue& doc);

bool WinJsonConfigStore::parseAndValidateSettingsDoc(const std::string& jsonText, AppConfig& cfgOut, std::string& outErr) const {
    outErr.clear();
    JsonValue doc;
    std::string perr;
    if (!parseJson(jsonText, doc, perr)) {
        outErr = "JSON 解析失败: " + perr;
        return false;
    }
    // 迁移过渡（FACE-ARCWIN-006/007）：先补齐 recognition 段缺键，再做 required 收束后的 schema 校验。
    // 否则既有缺 identifyThresholdArcFace 的 config.json 会被拒，进而降级为默认值运行（历史行为更糟：
    // 穿透 INI 分支覆盖用户配置；FACE-CONFIG-001 已改为先隔离坏文件再以默认值启动）。
    normalizeRecognitionKeys(doc);
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
                cfg.poster.postUrl.assign(reinterpret_cast<const char*>(plain.data()), plain.size());
            }
        }

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

// FACE-ARCWIN-006/007 迁移过渡：键级规范化——**只补本批新收束进 required 的那一个键**。
//
// 为什么需要：identifyThresholdArcFace 已进入 schema.required（见 JsonSchemaValidator.cpp），而既有部署的
// config.json 没有该键（它随本批才进入序列化）。若不先补齐，老配置会在启动校验被拒 ⇒ 降级为默认值运行。
// 历史行为（修复前）：initialize 在校验失败时并不 return，而是穿透到 INI 迁移分支、以 INI/默认值
// **覆盖写 config.json**（仅留一条 warn），用户配置即被丢弃。
// 现行为（FACE-CONFIG-001）：校验被拒时先把坏配置改名另存再以默认值启动，不再覆盖销毁；但缺键的既有
// 配置仍会降级运行，故仍须在此补齐，使其走「加载成功」路径。
//
// 为什么**只补这一个键**（刻意不推广到 recognition 段其余键）：其余 8 个键的接受/拒绝语义属历史契约，
// 本批不应改动。若一并补默认，「删除 cascadePath 等必填键」会从「校验失败 ⇒ 回滚 .bak」静默变成
// 「以默认值接受」——属未声明的行为放宽。补入值与解析侧「键缺失保留结构体默认」同源，故本规范化
// 不改变任何**已被接受**配置的解析结果，只让新增的 required 校验能通过。
static void normalizeRecognitionKeys(JsonValue& doc) {
    JsonValue* rec = doc.find("recognition");
    if (!rec || !rec->isObject()) return;
    if (!rec->find("identifyThresholdArcFace")) {
        rec->o["identifyThresholdArcFace"] =
            JsonValue::makeNumber(defaultAppConfig().recognition.identifyThresholdArcFace);
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

    // 读取现有 config.json
    if (std::filesystem::exists(configPath_)) {
        std::string txt, err;
        if (readFileAll(configPath_, txt, err)) {
            AppConfig cfg;
            if (parseAndValidateSettingsDoc(txt, cfg, err)) {
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
                    if (!writeAtomicallyWithBackup(prettyMigrated, writeErr)) {
                        // 只改了内存 cascadePath；写盘失败 ⇒ 磁盘仍是失效的旧路径，下次启动会再次进入本分支。
                        migrateWarn = "级联路径迁移未持久化（写入 config.json 失败）: " + writeErr;
                    }
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
        if (!writeAtomicallyWithBackup(pretty, writeErr)) {
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

    // 基于已知落盘的纯净 JSON（解耦环境覆盖项），深度合并 patch，再校验+落盘
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

    // 落盘：敏感字段强制加密存储
    const std::string prettyEnc = buildSettingsJson(newCfg, false, true);
    if (prettyEnc.find("\"_error\"") != std::string::npos) {
        r.httpStatus = 500;
        r.code = "crypto_error";
        r.message = "敏感字段加密失败";
        r.details.push_back(prettyEnc);
        return r;
    }
    // ★ 并发互斥（FACE-CONFIG-010 竞态收口）：写盘与内存提交（cfg_/generation_）必须在**同一** mu_ 临界区内，
    //   以便与 reloadFromDisk 回滚段的「取快照 → 序列化 → 写回磁盘」互斥，杜绝「回滚线程用旧快照覆盖已提交的
    //   新 PUT」这一竞态（详见 reloadFromDisk 回滚处的同因注释）。
    //   为何可直接扩大 mu_（而非新增专用写锁）：buildSettingsJson / writeAtomicallyWithBackup **均不取 mu_**
    //   （逐字核实），故持 mu_ 调用它们不会递归加锁；全函数仅此一把锁 ⇒ 无锁序问题、无死锁风险。
    //   代价：写盘期间 mu_ 被持有 ⇒ current() 短暂阻塞（PUT 为低频管理操作，可接受）。
    {
        std::lock_guard<std::mutex> lock(mu_);
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

    AppConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg = cfg_;
    }

    const bool hadOldKey = std::filesystem::exists(keyPath_);
    std::string oldKeyB64;
    std::string ioErr;
    if (hadOldKey) {
        if (!readFileAll(keyPath_, oldKeyB64, ioErr)) {
            r.code = "io_error";
            r.message = "读取旧密钥失败";
            r.details.push_back(ioErr);
            return r;
        }
    }

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
    if (!ensureParentDir(keyPath_, ioErr)) {
        r.code = "io_error";
        r.message = "创建密钥目录失败";
        r.details.push_back(ioErr);
        return r;
    }
    if (!writeFileAll(keyPath_, newKeyB64, ioErr)) {
        r.code = "io_error";
        r.message = "写入新密钥失败";
        r.details.push_back(ioErr);
        return r;
    }

    std::string err;
    const std::string prettyEnc = buildSettingsJson(cfg, false, true);
    if (prettyEnc.find("\"_error\"") != std::string::npos) {
        if (hadOldKey) {
            (void)writeFileAll(keyPath_, oldKeyB64, ioErr);
        } else {
            std::error_code ec;
            std::filesystem::remove(keyPath_, ec);
        }
        r.code = "crypto_error";
        r.message = "使用新密钥加密失败";
        r.details.push_back(prettyEnc);
        return r;
    }
    // ★ 并发互斥（FACE-CONFIG-010 竞态收口）：写盘 + 内存提交同处一个 mu_ 临界区，与 PUT / 热重载回滚写回互斥，
    //   杜绝「回滚线程用旧快照覆盖已提交的新配置」。writeAtomicallyWithBackup / writeFileAll **均不取 mu_**
    //   （逐字核实）⇒ 无递归加锁；本函数仅此一把锁 ⇒ 无锁序问题。
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!writeAtomicallyWithBackup(prettyEnc, err)) {
            if (hadOldKey) {
                (void)writeFileAll(keyPath_, oldKeyB64, ioErr);
            } else {
                std::error_code ec;
                std::filesystem::remove(keyPath_, ec);
            }
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

    r.ok = true;
    r.httpStatus = 200;
    r.code = "ok";
    r.message = "OK";
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
            // 记录**已观察到的 mtime**：文件持续不可读（权限 / 独占占用 / 半写）时，若不记录，
            // 1 秒轮询会反复重读同一份不可读文件并反复失败（IO / 日志噪声）。记录后
            // lastWriteTime_ == wt ⇒ 下次轮询在上一行直接早退，等价于「等待真正的下一次变更」。
            lastWriteTime_ = wt;
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
        outErr = "热重载校验失败: " + parseErr;
        std::filesystem::path movedTo;
        std::string qerr;
        if (!quarantineConfigFile(configPath_, movedTo, qerr)) {
            // 隔离失败 ⇒ 坏文件仍在原位。记录**已观察到的 mtime**，否则 1 秒轮询会反复重读同一份
            // 坏文件并反复失败（IO / 日志噪声）。
            outErr += "; 隔离失败（" + qerr + "），保留原文件";
            std::lock_guard<std::mutex> lock(mu_);
            std::error_code wec;
            lastWriteTime_ = std::filesystem::last_write_time(configPath_, wec);
            return false;
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
        // ── 并发互斥（FACE-CONFIG-010 竞态收口；本文件三处写盘路径统一用 mu_ 串行化）──
        // 以下「取内存快照 → 序列化 → 写回磁盘 → 记录 mtime」**整段**必须在 mu_ 临界区内完成，与
        // updateFromJsonBody（PUT）及 rotateKeyAndReencrypt 的「写盘 + 内存提交」互斥。
        // 否则失效时序（三路对抗审查逐字推演，已复核为真实可达）：回滚线程取到旧快照 A → 并发 PUT 成功
        //   写盘 B 且提交 cfg_=B、generation++ → 回滚线程继续把 A 写回 config.json（**覆盖 B**）→ 再把
        //   lastWriteTime_ 记为 A 的 mtime ⇒ 下一轮轮询因 mtime 相同早退 ⇒ 磁盘=A、内存=B 的分歧持久不
        //   收敛，重启读到的就是被撤销的 A。这与 FACE-CONFIG-010 声称消除的失败是**同一个**，只是从确定性
        //   版变成竞态版。
        // 为何直接用 mu_ 而非新增专用写锁：mu_ 已是内存态的唯一锁；本段调用的 buildSettingsJson /
        //   serializedDocIsWritable / writeFileAll / moveReplaceWriteThrough / parseAndValidateSettingsDoc
        //   均**不取 mu_**（逐字核实）⇒ 不会递归加锁；三处写盘路径都只用 mu_ 一把锁 ⇒ 无锁序问题、无死锁。
        //   代价：写盘期间 mu_ 被持有 ⇒ current() 短暂阻塞（本路径只在检测到外部坏写时触发，罕见）。
        // rbTmp 为**固定名** `config.json.rollback.tmp`：写入 + moveReplaceWriteThrough 成功即消失；但进程
        //   若在写盘中途崩溃**会残留**该文件（initialize 与 writeAtomicallyWithBackup 都不清理它）。残留无
        //   正确性影响（下次回滚以 trunc 覆盖），仅占一份磁盘空间，属已知登记项（见 config.md）。
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
