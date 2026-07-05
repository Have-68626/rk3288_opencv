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
#include <optional>
#include <sstream>

namespace rk_win {
namespace {

[[maybe_unused]] constexpr const wchar_t* kProductNameW = L"rk_wcfr"; // Reserved for future visible product/app name
constexpr const wchar_t* kConfigDirNameW = L"rk_wcfr";                // Internal identifier for local filesystem paths
constexpr int kInferenceIntervalDefaultMs = 150;
constexpr int kInferenceIntervalMinMs = 80;
constexpr int kInferenceIntervalMaxMs = 500;

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
        r.o["minFaceSizePx"] = JsonValue::makeNumber(cfg.recognition.minFaceSizePx);
        r.o["identifyThreshold"] = JsonValue::makeNumber(cfg.recognition.identifyThreshold);
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
            p.o["postUrl"] = JsonValue::makeString("***");
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

bool WinJsonConfigStore::parseAndValidateSettingsDoc(const std::string& jsonText, AppConfig& cfgOut, std::string& outErr) const {
    outErr.clear();
    JsonValue doc;
    std::string perr;
    if (!parseJson(jsonText, doc, perr)) {
        outErr = "JSON 解析失败: " + perr;
        return false;
    }
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
        if (getNumber(*r, "minFaceSizePx", v)) cfg.recognition.minFaceSizePx = static_cast<int>(v);
        if (getNumber(*r, "identifyThreshold", v)) cfg.recognition.identifyThreshold = v;
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

    // 读取现有 config.json
    if (std::filesystem::exists(configPath_)) {
        std::string txt, err;
        if (!readFileAll(configPath_, txt, err)) {
            warnOut = err;
        } else {
            AppConfig cfg;
            if (parseAndValidateSettingsDoc(txt, cfg, err)) {
                const auto oldCascade = resolvePathFromExeDir(L"tests/data/lbpcascade_frontalface.xml");
                if (cfg.recognition.cascadePath == oldCascade && !std::filesystem::exists(cfg.recognition.cascadePath)) {
                    cfg.recognition.cascadePath = resolvePathFromExeDir(L"assets/lbpcascade_frontalface.xml");
                    const std::string prettyMigrated = buildSettingsJson(cfg, false, true);
                    std::string writeErr;
                    (void)writeAtomicallyWithBackup(prettyMigrated, writeErr);
                }

                const std::string pretty = buildSettingsJson(cfg, false, true);
                std::lock_guard<std::mutex> lock(mu_);
                cfg_ = cfg;
                lastGoodJsonPretty_ = pretty;
                std::error_code ec;
                lastWriteTime_ = std::filesystem::last_write_time(configPath_, ec);
                return true;
            }
            warnOut = err;
        }
    }

    // 迁移：从旧 INI 读取（兼容已有流程），然后落盘为 JSON
    AppConfig cfg = loadConfigFromIniOrDefault();
    cfg.configPath = configPath_;
    std::string writeErr;
    const std::string pretty = buildSettingsJson(cfg, false, true);
    if (!writeAtomicallyWithBackup(pretty, writeErr)) {
        warnOut = warnOut.empty() ? ("写入默认 JSON 配置失败: " + writeErr) : (warnOut + "; " + writeErr);
        // 即使落盘失败，也允许继续用内存配置运行
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_ = cfg;
        lastGoodJsonPretty_ = pretty;
        std::error_code ec;
        lastWriteTime_ = std::filesystem::exists(configPath_) ? std::filesystem::last_write_time(configPath_, ec) : std::filesystem::file_time_type{};
    }
    return true;
}

AppConfig WinJsonConfigStore::current() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_;
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
    if (!writeAtomicallyWithBackup(prettyEnc, err)) {
        r.httpStatus = 500;
        r.code = "io_error";
        r.message = "写入配置失败";
        r.details.push_back(err);
        return r;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_ = newCfg;
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

    {
        std::lock_guard<std::mutex> lock(mu_);
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
        if (!readFileAll(configPath_, txt, outErr)) return false;
    }

    AppConfig cfg;
    std::string parseErr;
    if (!parseAndValidateSettingsDoc(txt, cfg, parseErr)) {
        // 回滚：尝试使用 .bak
        outErr = "热重载校验失败，将尝试回滚: " + parseErr;
        if (std::filesystem::exists(bakPath_)) {
            std::string bakTxt, e2;
            if (readFileAll(bakPath_, bakTxt, e2)) {
                AppConfig bakCfg;
                if (parseAndValidateSettingsDoc(bakTxt, bakCfg, e2)) {
                    // 恢复 .bak -> config.json
                    std::string ioErr;
                    const std::filesystem::path tmp = configPath_.parent_path() / L"config.json.rollback.tmp";
                    if (writeFileAll(tmp, bakTxt, ioErr) && moveReplaceWriteThrough(tmp, configPath_, ioErr)) {
                        cfg = bakCfg;
                        txt = bakTxt;
                    } else {
                        outErr += "; 回滚写入失败: " + ioErr;
                        return false;
                    }
                } else {
                    outErr += "; .bak 也无效: " + e2;
                    return false;
                }
            } else {
                outErr += "; 读取 .bak 失败: " + e2;
                return false;
            }
        } else {
            return false;
        }
    }

    cfg.configPath = configPath_;
    const std::string pretty = buildSettingsJson(cfg, false, true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto currentWt = std::filesystem::last_write_time(configPath_, ec);
        if (!ec && currentWt != wt) {
            // 文件在读取后被再次修改，放弃本次更新以避免覆盖
            outErr = "配置在读取期间被外部修改，放弃热重载";
            return false;
        }
        cfg_ = cfg;
        lastGoodJsonPretty_ = pretty;
        lastWriteTime_ = wt;
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
