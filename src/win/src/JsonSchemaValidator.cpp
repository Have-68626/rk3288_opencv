#include "rk_win/JsonSchemaValidator.h"

#include <string>
#include <vector>

namespace rk_win {
namespace {

static std::string typeName(JsonValue::Type t) {
    switch (t) {
        case JsonValue::Type::Null:   return "null";
        case JsonValue::Type::Bool:   return "boolean";
        case JsonValue::Type::Number: return "number";
        case JsonValue::Type::String: return "string";
        case JsonValue::Type::Object: return "object";
        case JsonValue::Type::Array:  return "array";
    }
    return "unknown";
}

static bool schemaAcceptType(const JsonValue& schemaType, JsonValue::Type instanceType) {
    auto matches = [&](const std::string& t) -> bool {
        if (t == "null")    return instanceType == JsonValue::Type::Null;
        if (t == "boolean") return instanceType == JsonValue::Type::Bool;
        if (t == "number")  return instanceType == JsonValue::Type::Number;
        if (t == "string")  return instanceType == JsonValue::Type::String;
        if (t == "object")  return instanceType == JsonValue::Type::Object;
        if (t == "array")   return instanceType == JsonValue::Type::Array;
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

}  // anonymous namespace

// --- JsonSchemaValidator ---

bool JsonSchemaValidator::validate(const JsonValue& schema, const JsonValue& doc, std::string* errorOut) {
    std::vector<std::string> errs;
    validateSchemaRec(schema, doc, "", errs);
    if (errorOut && !errs.empty()) {
        for (size_t i = 0; i < errs.size(); ++i) {
            if (i > 0) *errorOut += "; ";
            *errorOut += errs[i];
        }
    }
    return errs.empty();
}

JsonValue JsonSchemaValidator::buildSettingsDocSchema() {
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
        req.a.push_back(JsonValue::makeString("arcFaceModelPath"));
        req.a.push_back(JsonValue::makeString("minFaceSizePx"));
        req.a.push_back(JsonValue::makeString("identifyThreshold"));
        req.a.push_back(JsonValue::makeString("enrollSamples"));
        r.o["required"] = std::move(req);
        JsonValue rp = JsonValue::makeObject();
        rp.o["cascadePath"] = objStr();
        rp.o["databasePath"] = objStr();
        rp.o["arcFaceModelPath"] = objStr();
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

    // inference（可选：为了兼容旧 config.json，这个字段不加入 root.required）
    {
        JsonValue i = JsonValue::makeObject();
        i.o["type"] = JsonValue::makeString("object");
        i.o["additionalProperties"] = JsonValue::makeBool(false);
        JsonValue req = JsonValue::makeArray();
        req.a.push_back(JsonValue::makeString("throttleMode"));
        req.a.push_back(JsonValue::makeString("intervalMs"));
        i.o["required"] = std::move(req);
        JsonValue ip = JsonValue::makeObject();
        ip.o["throttleMode"] = objStr();
        ip.o["intervalMs"] = objBoolInt(true, 80, 500);
        i.o["properties"] = std::move(ip);
        props.o["inference"] = std::move(i);
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
        dp.o["inputWidth"] = objBoolInt(true, 1, 8192);
        dp.o["inputHeight"] = objBoolInt(true, 1, 8192);
        dp.o["scale"] = objBoolInt(false, 0, 0);
        dp.o["meanB"] = objBoolInt(false, 0, 0);
        dp.o["meanG"] = objBoolInt(false, 0, 0);
        dp.o["meanR"] = objBoolInt(false, 0, 0);
        dp.o["swapRB"] = objBool();
        dp.o["confThreshold"] = objBoolInt(true, 0.0, 1.0);
        dp.o["backend"] = objBoolInt(false, 0, 0);
        dp.o["target"] = objBoolInt(false, 0, 0);
        d.o["properties"] = std::move(dp);
        props.o["dnn"] = std::move(d);
    }

    // model (optional, with defaults)
    {
        JsonValue m = JsonValue::makeObject();
        m.o["type"] = JsonValue::makeString("object");
        m.o["additionalProperties"] = JsonValue::makeBool(false);
        JsonValue mp = JsonValue::makeObject();
        mp.o["detection"] = objStr();
        mp.o["recognition"] = objStr();
        mp.o["backend"] = objStr();
        mp.o["detectorBackend"] = objStr();
        mp.o["recognitionBackend"] = objStr();
        mp.o["autoFallback"] = objBool();
        m.o["properties"] = std::move(mp);
        props.o["model"] = std::move(m);
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
        dp.o["width"] = objBoolInt(true, 0, 7680);
        dp.o["height"] = objBoolInt(true, 0, 4320);
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
        req.a.push_back(JsonValue::makeString("enableLibyuv"));
        req.a.push_back(JsonValue::makeString("enableMpp"));
        req.a.push_back(JsonValue::makeString("enableQualcomm"));
        a.o["required"] = std::move(req);
        JsonValue ap = JsonValue::makeObject();
        ap.o["enableOpenCL"] = objBool();
        ap.o["enableLibyuv"] = objBool();
        ap.o["enableMpp"] = objBool();
        ap.o["enableQualcomm"] = objBool();
        a.o["properties"] = std::move(ap);
        props.o["acceleration"] = std::move(a);
    }

    schema.o["properties"] = std::move(props);
    return schema;
}

JsonValue JsonSchemaValidator::buildEncryptedStringSchema() {
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

}  // namespace rk_win
