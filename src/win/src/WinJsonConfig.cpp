#include "rk_win/WinJsonConfig.h"

#include "rk_win/JsonLite.h"
#include "rk_win/WinCrypto.h"

#ifdef _WIN32
#include <windows.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#endif

#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>

namespace rk_win {
namespace {

constexpr const wchar_t* kProductNameW = L"rk_wcfr";  // TODO: 产品化时可替换为品牌名/应用名

std::wstring getEnvW(const wchar_t* name) {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return L"";
    return std::wstring(buf, buf + n);
#else
    (void)name;
    return L"";
#endif
}

std::string utf8FromWide(const std::wstring& ws) {
    if (ws.empty()) return {};
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
#else
    return std::string(ws.begin(), ws.end());
#endif
}

std::wstring wideFromUtf8(const std::string& s) {
    if (s.empty()) return L"";
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

bool readFileAll(const std::filesystem::path& p, std::string& out, std::string& err) {
    err.clear();
    out.clear();
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) {
        err = "无法读取文件: " + p.string();
        return false;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
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
        std::error_code ec;
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            err = "MoveFileExW(REPLACE) 失败，且 copy_file 回退失败";
            return false;
        }
        std::filesystem::remove(src, ec);
        return true;
    }
    return true;
#else
    (void)src;
    (void)dst;
    err = "平台不支持原子替换";
    return false;
#endif
}

// --- schema（JSON Schema 子集）校验 ---
static JsonValue schemaEncryptedString() {
    JsonValue s = JsonValue::makeObject();
    s.o["type"] = JsonValue::makeString("object");
    s.o["additionalProperties"] = JsonValue::makeBool(false);

    JsonValue req = JsonValue::makeArray();
    req.a.push_back(JsonValue::makeString("v"));
    req.a.push_back(JsonValue::makeString("alg"));
    req.a.push_back(JsonValue::makeString("nonce"));
    req.a.push_back(JsonValue::makeString("ciphertext"));
    req.a.push_back(JsonValue::makeString("tag"));
    s.o["required"] = std::move(req);

    JsonValue props = JsonValue::makeObject();
    {
        JsonValue v = JsonValue::makeObject();
        v.o["type"] = JsonValue::makeString("number");
        v.o["minimum"] = JsonValue::makeNumber(1);
        v.o["maximum"] = JsonValue::makeNumber(1);
        props.o["v"] = std::move(v);
    }
    {
        JsonValue alg = JsonValue::makeObject();
        alg.o["type"] = JsonValue::makeString("string");
        props.o["alg"] = std::move(alg);
    }
    {
        JsonValue f = JsonValue::makeObject();
        f.o["type"] = JsonValue::makeString("string");
        props.o["nonce"] = f;
        props.o["ciphertext"] = f;
        props.o["tag"] = f;
    }
    s.o["properties"] = std::move(props);
    return s;
}

static JsonValue schemaSettingsDoc() {
    // 注意：这里只实现项目当前需要的字段。新增字段时务必同步更新 schema + fromJson/toJson。
    JsonValue schema = JsonValue::makeObject();
    schema.o["type"] = JsonValue::makeString("object");
    schema.o["additionalProperties"] = JsonValue::makeBool(false);

    JsonValue required = JsonValue::makeArray();
    required.a.push_back(JsonValue::makeString("schemaVersion"));
    required.a.push_back(JsonValue::makeString("camera"));
    required.a.push_back(JsonValue::makeString("recognition"));
    required.a.push_back(JsonValue::makeString("dnn"));
    required.a.push_back(JsonValue::makeString("http"));
    required.a.push_back(JsonValue::makeString("poster"));
    required.a.push_back(JsonValue::makeString("log"));
    required.a.push_back(JsonValue::makeString("ui"));
    required.a.push_back(JsonValue::makeString("display"));
    schema.o["required"] = std::move(required);

    JsonValue props = JsonValue::makeObject();
    {
        JsonValue v = JsonValue::makeObject();
        v.o["type"] = JsonValue::makeString("number");
        v.o["minimum"] = JsonValue::makeNumber(1);
        v.o["maximum"] = JsonValue::makeNumber(1);
        props.o["schemaVersion"] = std::move(v);
    }
    auto objBoolInt = [](bool hasMinMax, double minV, double maxV) {
        JsonValue t = JsonValue::makeObject();
        t.o["type"] = JsonValue::makeString("number");
        if (hasMinMax) {
            t.o["minimum"] = JsonValue::makeNumber(minV);
            t.o["maximum"] = JsonValue::makeNumber(maxV);
        }
        return t;
    };
    auto objBool = []() {
        JsonValue t = JsonValue::makeObject();
        t.o["type"] = JsonValue::makeString("boolean");
        return t;
    };
    auto objStr = []() {
        JsonValue t = JsonValue::makeObject();
        t.o["type"] = JsonValue::makeString("string");
        return t;
    };

    // camera
    {
        JsonValue cam = JsonValue::makeObject();
        cam.o["type"] = JsonValue::makeString("object");
        cam.o["additionalProperties"] = JsonValue::makeBool(false);
        JsonValue camReq = JsonValue::makeArray();
        camReq.a.push_back(JsonValue::makeString("preferredDeviceId"));
        camReq.a.push_back(JsonValue::makeString("width"));
        camReq.a.push_back(JsonValue::makeString("height"));
        camReq.a.push_back(JsonValue::makeString("fps"));
        cam.o["required"] = std::move(camReq);
        JsonValue camProps = JsonValue::makeObject();
        camProps.o["preferredDeviceId"] = objStr();
        camProps.o["width"] = objBoolInt(true, 1, 8192);
        camProps.o["height"] = objBoolInt(true, 1, 8192);
        camProps.o["fps"] = objBoolInt(true, 1, 240);
        cam.o["properties"] = std::move(camProps);
        props.o["camera"] = std::move(cam);
    }

    // recognition
    {
        JsonValue r = JsonValue::makeObject();
        r.o["type"] = JsonValue::makeString("object");
        r.o["additionalProperties"] = JsonValue::makeBool(false);
        JsonValue req = JsonValue::makeArray();
        req.a.push_back(JsonValue::makeString("cascadePath"));
        req.a.push_back(JsonValue::makeString("databasePath"));
        req.a.push_back(JsonValue::makeString("minFaceSizePx"));
        req.a.push_back(JsonValue::makeString("identifyThreshold"));
        req.a.push_back(JsonValue::makeString("enrollSamples"));
        r.o["required"] = std::move(req);
        JsonValue rp = JsonValue::makeObject();
        rp.o["cascadePath"] = objStr();
        rp.o["databasePath"] = objStr();
        rp.o["minFaceSizePx"] = objBoolInt(true, 10, 10000);
        {
            JsonValue t = JsonValue::makeObject();
            t.o["type"] = JsonValue::makeString("number");
            t.o["minimum"] = JsonValue::makeNumber(1.0);
            t.o["maximum"] = JsonValue::makeNumber(100000.0);
            rp.o["identifyThreshold"] = std::move(t);
        }
        rp.o["enrollSamples"] = objBoolInt(true, 1, 1000);
        r.o["properties"] = std::move(rp);
        props.o["recognition"] = std::move(r);
    }

    // dnn
    {
        JsonValue d = JsonValue::makeObject();
        d.o["type"] = JsonValue::makeString("object");
        d.o["additionalProperties"] = JsonValue::makeBool(false);
        JsonValue req = JsonValue::makeArray();
        req.a.push_back(JsonValue::makeString("enable"));
        req.a.push_back(JsonValue::makeString("modelPath"));
        req.a.push_back(JsonValue::makeString("configPath"));
        req.a.push_back(JsonValue::makeString("inputWidth"));
        req.a.push_back(JsonValue::makeString("inputHeight"));
        req.a.push_back(JsonValue::makeString("scale"));
        req.a.push_back(JsonValue::makeString("meanB"));
        req.a.push_back(JsonValue::makeString("meanG"));
        req.a.push_back(JsonValue::makeString("meanR"));
        req.a.push_back(JsonValue::makeString("swapRB"));
        req.a.push_back(JsonValue::makeString("confThreshold"));
        req.a.push_back(JsonValue::makeString("backend"));
        req.a.push_back(JsonValue::makeString("target"));
        d.o["required"] = std::move(req);
        JsonValue dp = JsonValue::makeObject();
        dp.o["enable"] = objBool();
        dp.o["modelPath"] = objStr();
        dp.o["configPath"] = objStr();
        dp.o["inputWidth"] = objBoolInt(true, 1, 4096);
        dp.o["inputHeight"] = objBoolInt(true, 1, 4096);
        dp.o["scale"] = objBoolInt(false, 0, 0);  // number，无范围
        dp.o["meanB"] = objBoolInt(true, 0, 255);
        dp.o["meanG"] = objBoolInt(true, 0, 255);
        dp.o["meanR"] = objBoolInt(true, 0, 255);
        dp.o["swapRB"] = objBool();
        dp.o["confThreshold"] = objBoolInt(true, 0.0, 1.0);
        dp.o["backend"] = objBoolInt(true, 0, 999);
        dp.o["target"] = objBoolInt(true, 0, 999);
        d.o["properties"] = std::move(dp);
        props.o["dnn"] = std::move(d);
    }

    // http
    {
        JsonValue h = JsonValue::makeObject();
        h.o["type"] = JsonValue::makeString("object");
        h.o["additionalProperties"] = JsonValue::makeBool(false);
        JsonValue req = JsonValue::makeArray();
        req.a.push_back(JsonValue::makeString("enable"));
        req.a.push_back(JsonValue::makeString("port"));
        h.o["required"] = std::move(req);
        JsonValue hp = JsonValue::makeObject();
        hp.o["enable"] = objBool();
        hp.o["port"] = objBoolInt(true, 1, 65535);
        h.o["properties"] = std::move(hp);
        props.o["http"] = std::move(h);
    }

    // poster（postUrl 为敏感字段：允许 string(明文) 或 encrypted-object）
    {
        JsonValue p = JsonValue::makeObject();
        p.o["type"] = JsonValue::makeString("object");
        p.o["additionalProperties"] = JsonValue::makeBool(false);
        JsonValue req = JsonValue::makeArray();
        req.a.push_back(JsonValue::makeString("enable"));
        req.a.push_back(JsonValue::makeString("postUrl"));
        req.a.push_back(JsonValue::makeString("throttleMs"));
        req.a.push_back(JsonValue::makeString("backoffMinMs"));
        req.a.push_back(JsonValue::makeString("backoffMaxMs"));
        p.o["required"] = std::move(req);
        JsonValue pp = JsonValue::makeObject();
        pp.o["enable"] = objBool();
        {
            // type: ["string","object"]
            JsonValue t = JsonValue::makeObject();
            JsonValue types = JsonValue::makeArray();
            types.a.push_back(JsonValue::makeString("string"));
            types.a.push_back(JsonValue::makeString("object"));
            t.o["type"] = std::move(types);
            // object 分支再用 allOf 模拟太重，这里靠业务逻辑二次判断。
            pp.o["postUrl"] = std::move(t);
        }
        pp.o["throttleMs"] = objBoolInt(true, 0, 60 * 60 * 1000);
        pp.o["backoffMinMs"] = objBoolInt(true, 50, 60 * 60 * 1000);
        pp.o["backoffMaxMs"] = objBoolInt(true, 50, 60 * 60 * 1000);
        p.o["properties"] = std::move(pp);
        props.o["poster"] = std::move(p);
    }

    // log
    {
        JsonValue l = JsonValue::makeObject();
        l.o["type"] = JsonValue::makeString("object");
        l.o["additionalProperties"] = JsonValue::makeBool(false);
        JsonValue req = JsonValue::makeArray();
        req.a.push_back(JsonValue::makeString("logDir"));
        req.a.push_back(JsonValue::makeString("maxFileBytes"));
        req.a.push_back(JsonValue::makeString("maxRollFiles"));
        l.o["required"] = std::move(req);
        JsonValue lp = JsonValue::makeObject();
        lp.o["logDir"] = objStr();
        lp.o["maxFileBytes"] = objBoolInt(true, 1024, 1073741824.0);
        lp.o["maxRollFiles"] = objBoolInt(true, 1, 1000);
        l.o["properties"] = std::move(lp);
        props.o["log"] = std::move(l);
    }

    // ui
    {
        JsonValue u = JsonValue::makeObject();
        u.o["type"] = JsonValue::makeString("object");
        u.o["additionalProperties"] = JsonValue::makeBool(false);
        JsonValue req = JsonValue::makeArray();
        req.a.push_back(JsonValue::makeString("windowWidth"));
        req.a.push_back(JsonValue::makeString("windowHeight"));
        req.a.push_back(JsonValue::makeString("previewScaleMode"));
        u.o["required"] = std::move(req);
        JsonValue up = JsonValue::makeObject();
        up.o["windowWidth"] = objBoolInt(true, 320, 7680);
        up.o["windowHeight"] = objBoolInt(true, 240, 4320);
        up.o["previewScaleMode"] = objBoolInt(true, 0, 1);
        u.o["properties"] = std::move(up);
        props.o["ui"] = std::move(u);
    }

    // display
    {
        JsonValue d = JsonValue::makeObject();
        d.o["type"] = JsonValue::makeString("object");
        d.o["additionalProperties"] = JsonValue::makeBool(false);
        JsonValue req = JsonValue::makeArray();
        for (const char* k : {"outputIndex", "width", "height", "refreshNumerator", "refreshDenominator", "vsync", "swapchainBuffers",
                              "fullscreen", "allowSystemModeSwitch", "enableSRGB", "gamma", "colorTempK", "aaSamples", "anisoLevel"}) {
            req.a.push_back(JsonValue::makeString(k));
        }
        d.o["required"] = std::move(req);
        JsonValue dp = JsonValue::makeObject();
        dp.o["outputIndex"] = objBoolInt(true, 0, 32);
        dp.o["width"] = objBoolInt(true, 0, 20000);
        dp.o["height"] = objBoolInt(true, 0, 20000);
        dp.o["refreshNumerator"] = objBoolInt(true, 0, 1000000);
        dp.o["refreshDenominator"] = objBoolInt(true, 1, 1000000);
        dp.o["vsync"] = objBool();
        dp.o["swapchainBuffers"] = objBoolInt(true, 1, 8);
        dp.o["fullscreen"] = objBool();
        dp.o["allowSystemModeSwitch"] = objBool();
        dp.o["enableSRGB"] = objBool();
        dp.o["gamma"] = objBoolInt(true, 0.1, 10.0);
        dp.o["colorTempK"] = objBoolInt(true, 1000, 20000);
        dp.o["aaSamples"] = objBoolInt(true, 1, 16);
        dp.o["anisoLevel"] = objBoolInt(true, 1, 16);
        d.o["properties"] = std::move(dp);
        props.o["display"] = std::move(d);
    }

    // acceleration
    {
        JsonValue a = JsonValue::makeObject();
        a.o["type"] = JsonValue::makeString("object");
        a.o["additionalProperties"] = JsonValue::makeBool(false);
        JsonValue req = JsonValue::makeArray();
        req.a.push_back(JsonValue::makeString("enableOpenCL"));
        req.a.push_back(JsonValue::makeString("enableMpp"));
        req.a.push_back(JsonValue::makeString("enableQualcomm"));
        a.o["required"] = std::move(req);
        JsonValue ap = JsonValue::makeObject();
        ap.o["enableOpenCL"] = objBool();
        ap.o["enableMpp"] = objBool();
        ap.o["enableQualcomm"] = objBool();
        a.o["properties"] = std::move(ap);
        props.o["acceleration"] = std::move(a);
    }

    schema.o["properties"] = std::move(props);
    return schema;
}

static std::string typeName(JsonValue::Type t) {
    switch (t) {
        case JsonValue::Type::Null: return "null";
        case JsonValue::Type::Bool: return "boolean";
        case JsonValue::Type::Number: return "number";
        case JsonValue::Type::String: return "string";
        case JsonValue::Type::Object: return "object";
        case JsonValue::Type::Array: return "array";
    }
    return "unknown";
}

static bool schemaAcceptType(const JsonValue& schemaType, JsonValue::Type instanceType) {
    auto matches = [&](const std::string& t) -> bool {
        if (t == "null") return instanceType == JsonValue::Type::Null;
        if (t == "boolean") return instanceType == JsonValue::Type::Bool;
        if (t == "number") return instanceType == JsonValue::Type::Number;
        if (t == "string") return instanceType == JsonValue::Type::String;
        if (t == "object") return instanceType == JsonValue::Type::Object;
        if (t == "array") return instanceType == JsonValue::Type::Array;
        return false;
    };

    if (schemaType.isString()) return matches(schemaType.s);
    if (schemaType.isArray()) {
        for (const auto& it : schemaType.a) {
            if (it.isString() && matches(it.s)) return true;
        }
        return false;
    }
    return true;  // 没声明 type 则放行
}

static void validateSchemaRec(const JsonValue& schema, const JsonValue& inst, const std::string& path, std::vector<std::string>& errs) {
    const JsonValue* type = schema.find("type");
    if (type && !schemaAcceptType(*type, inst.type)) {
        errs.push_back(path + ": 期望类型 " + (type->isString() ? type->s : "联合类型") + "，实际为 " + typeName(inst.type));
        return;
    }

    if (inst.isNumber()) {
        if (const JsonValue* minv = schema.find("minimum"); minv && minv->isNumber()) {
            if (inst.n < minv->n) errs.push_back(path + ": 过小，最小值=" + std::to_string(minv->n));
        }
        if (const JsonValue* maxv = schema.find("maximum"); maxv && maxv->isNumber()) {
            if (inst.n > maxv->n) errs.push_back(path + ": 过大，最大值=" + std::to_string(maxv->n));
        }
    }

    if (inst.isObject()) {
        // required
        if (const JsonValue* req = schema.find("required"); req && req->isArray()) {
            for (const auto& k : req->a) {
                if (!k.isString()) continue;
                if (!inst.find(k.s)) errs.push_back(path + ": 缺少必填字段 " + k.s);
            }
        }

        const JsonValue* props = schema.find("properties");
        const bool noExtra = (schema.find("additionalProperties") && schema.find("additionalProperties")->isBool() &&
                              schema.find("additionalProperties")->b == false);

        if (props && props->isObject()) {
            for (const auto& kv : props->o) {
                const auto* child = inst.find(kv.first);
                if (!child) continue;
                validateSchemaRec(kv.second, *child, path + "/" + kv.first, errs);
            }
        }
        if (noExtra && props && props->isObject()) {
            for (const auto& kv : inst.o) {
                if (props->o.find(kv.first) == props->o.end()) errs.push_back(path + ": 不允许的字段 " + kv.first);
            }
        }
    }

    if (inst.isArray()) {
        // 当前用不到 items 校验，留空即可；需要时再补。
    }
}

static bool validateSettingsSchema(const JsonValue& doc, std::vector<std::string>& errs) {
    errs.clear();
    const JsonValue schema = schemaSettingsDoc();
    validateSchemaRec(schema, doc, "", errs);

    // poster.postUrl：若为 object，必须满足 encrypted-object 结构
    if (const JsonValue* poster = doc.find("poster"); poster && poster->isObject()) {
        if (const JsonValue* pu = poster->find("postUrl"); pu && pu->isObject()) {
            const JsonValue encSchema = schemaEncryptedString();
            validateSchemaRec(encSchema, *pu, "/poster/postUrl", errs);
            if (const JsonValue* alg = pu->find("alg"); alg && alg->isString()) {
                if (alg->s != "AES-256-GCM") errs.push_back("/poster/postUrl/alg: 仅支持 AES-256-GCM");
            }
        }
    }
    return errs.empty();
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
    if (!envPath.empty()) return std::filesystem::path(envPath);

    const std::wstring appdata = getEnvW(L"APPDATA");
    std::filesystem::path base = appdata.empty() ? std::filesystem::current_path() : std::filesystem::path(appdata);
    base /= kProductNameW;
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
        r.o["minFaceSizePx"] = JsonValue::makeNumber(cfg.recognition.minFaceSizePx);
        r.o["identifyThreshold"] = JsonValue::makeNumber(cfg.recognition.identifyThreshold);
        r.o["enrollSamples"] = JsonValue::makeNumber(cfg.recognition.enrollSamples);
        root.o["recognition"] = std::move(r);
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
            if (!aes256gcmEncrypt(*keyOpt, plainBin, ct, e)) {
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
    std::vector<std::string> errs;
    if (!validateSettingsSchema(doc, errs)) {
        outErr = "Schema 校验失败: " + (errs.empty() ? std::string("unknown") : errs.front());
        return false;
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
        if (getNumber(*r, "minFaceSizePx", v)) cfg.recognition.minFaceSizePx = static_cast<int>(v);
        if (getNumber(*r, "identifyThreshold", v)) cfg.recognition.identifyThreshold = v;
        if (getNumber(*r, "enrollSamples", v)) cfg.recognition.enrollSamples = static_cast<int>(v);
    }

    // dnn
    if (const JsonValue* d = doc.find("dnn"); d && d->isObject()) {
        std::string s;
        double v = 0;
        bool b = false;
        if (getBool(*d, "enable", b)) cfg.dnn.enable = b;
        if (getString(*d, "modelPath", s)) cfg.dnn.modelPath = resolvePathFromExeDir(s);
        if (getString(*d, "configPath", s)) cfg.dnn.configPath = resolvePathFromExeDir(s);
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
                if (!aes256gcmDecrypt(key, ct, plain, e)) {
                    outErr = "postUrl 解密失败: " + e;
                    return false;
                }
                cfg.poster.postUrl.assign(reinterpret_cast<const char*>(plain.data()), plain.size());
            }
        }
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
        if (getBool(*a, "enableMpp", b)) cfg.acceleration.enableMpp = b;
        if (getBool(*a, "enableQualcomm", b)) cfg.acceleration.enableQualcomm = b;
    }

    cfgOut = std::move(cfg);
    return true;
}

std::string WinJsonConfigStore::buildSettingsJson(const AppConfig& cfg, bool redacted, bool encryptSensitive) const {
    std::vector<std::uint8_t> key;
    std::string err;
    if (encryptSensitive && !ensureOrLoadKey(key, err)) {
        // 返回最小 JSON，避免崩溃；错误由调用方决定是否记录/返回
        JsonValue o = JsonValue::makeObject();
        o.o["schemaVersion"] = JsonValue::makeNumber(1);
        o.o["_error"] = JsonValue::makeString("key_error: " + err);
        return toJsonString(o, true);
    }
    JsonValue doc = toSettingsDocObject(cfg, redacted, encryptSensitive, encryptSensitive ? &key : nullptr, err);
    if (!err.empty()) {
        JsonValue o = JsonValue::makeObject();
        o.o["schemaVersion"] = JsonValue::makeNumber(1);
        o.o["_error"] = JsonValue::makeString("encrypt_error: " + err);
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
    // 先备份旧文件
    if (std::filesystem::exists(configPath_)) {
        std::error_code ec;
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

    // 基于当前配置构造 doc，然后深度合并 patch，再校验+落盘
    AppConfig oldCfg;
    {
        std::lock_guard<std::mutex> lock(mu_);
        oldCfg = cfg_;
    }

    // 生成可编辑 doc（允许明文 postUrl 输入，落盘时会再加密）
    std::string err;
    std::vector<std::uint8_t> key;
    // 这里不需要 key 参与 doc 生成（明文可接受），但校验/解析需要解密时要 key
    JsonValue baseDoc = toSettingsDocObject(oldCfg, false, false, nullptr, err);
    if (!err.empty()) {
        r.httpStatus = 500;
        r.code = "internal_error";
        r.message = "构造 settings 失败";
        r.details.push_back(err);
        return r;
    }
    deepMergeObject(baseDoc, patch);

    // 校验 schema
    std::vector<std::string> errs;
    if (!validateSettingsSchema(baseDoc, errs)) {
        r.message = "Schema 校验失败";
        r.details = std::move(errs);
        return r;
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
    const auto wt = std::filesystem::last_write_time(configPath_, ec);
    if (ec) return true;

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (lastWriteTime_ == wt) return true;
    }

    std::string txt;
    if (!readFileAll(configPath_, txt, outErr)) return false;

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
        cfg_ = cfg;
        lastGoodJsonPretty_ = pretty;
        lastWriteTime_ = std::filesystem::last_write_time(configPath_, ec);
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
