#include "rk_win/JsonEndpointHandlers.h"
#include "rk_win/FramePipeline.h"
#include "rk_win/JsonLite.h"
#include "rk_win/WinJsonConfig.h"

#include <cctype>
#include <string>
#include <vector>

namespace rk_win {
namespace handlers {
namespace {

// ──── 帮助函数 ───────────────────────────────────────────────

using HttpResp = HttpFacesServer::HttpResponse;
using HttpReq = HttpFacesServer::HttpRequest;

/// 构建标准成功响应：{"ok":true, "data":...}
static HttpResp okJson(JsonValue data) {
    JsonValue root = JsonValue::makeObject();
    root.o["ok"] = JsonValue::makeBool(true);
    root.o["data"] = std::move(data);
    HttpResp r;
    r.status = 200;
    r.reason = "OK";
    r.contentType = "application/json; charset=utf-8";
    r.body = toJsonString(root, false);
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    r.headers.push_back({"Content-Security-Policy", "default-src 'none'"});
    return r;
}

/// 构建标准错误响应：{"ok":false, "error":{"code":..., "message":...}}
static HttpResp errJson(int httpStatus, const std::string& code, const std::string& message,
                        const std::vector<std::string>& details = {}) {
    JsonValue root = JsonValue::makeObject();
    root.o["ok"] = JsonValue::makeBool(false);
    JsonValue err = JsonValue::makeObject();
    err.o["code"] = JsonValue::makeString(code);
    err.o["message"] = JsonValue::makeString(message);
    if (!details.empty()) {
        JsonValue d = JsonValue::makeArray();
        for (const auto& x : details) d.a.push_back(JsonValue::makeString(x));
        err.o["details"] = std::move(d);
    }
    root.o["error"] = std::move(err);

    HttpResp r;
    r.status = httpStatus;
    r.reason = (httpStatus == 404 ? "Not Found" : httpStatus == 405 ? "Method Not Allowed" : "Error");
    r.contentType = "application/json; charset=utf-8";
    r.body = toJsonString(root, false);
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    r.headers.push_back({"Content-Security-Policy", "default-src 'none'"});
    return r;
}

static std::string trim(std::string s) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

}  // anonymous namespace

// ──── models ──────────────────────────────────────────────────

HttpResp handleGetModels(const HttpReq&, EndpointContext& ctx) {
    JsonValue d = JsonValue::makeObject();

    // 注册表中已知的受支持模型列表
    JsonValue supported = JsonValue::makeArray();
    {
        JsonValue s1 = JsonValue::makeObject();
        s1.o["id"] = JsonValue::makeString("cascade_frontalface");
        s1.o["displayName"] = JsonValue::makeString("Cascade Frontal Face (LBP)");
        s1.o["taskType"] = JsonValue::makeString("detect_recognize_pipeline");
        s1.o["notes"] = JsonValue::makeString("LBP 级联分类器，极轻量。适合低资源环境，精度低于 DNN 方案。Windows 管线默认检测器。");
        s1.o["recommendedFor"] = JsonValue::makeString("high_speed");
        supported.a.push_back(std::move(s1));

        JsonValue s2 = JsonValue::makeObject();
        s2.o["id"] = JsonValue::makeString("dnn_face_detector");
        s2.o["displayName"] = JsonValue::makeString("OpenCV DNN Face Detector");
        s2.o["taskType"] = JsonValue::makeString("detect");
        s2.o["notes"] = JsonValue::makeString("ResNet SSD 300x300，OpenCV DNN 后端检测器。精度高于 Cascade，适合 Windows 管线。");
        s2.o["recommendedFor"] = JsonValue::makeString("balanced");
        supported.a.push_back(std::move(s2));
    }
    d.o["supportedModels"] = std::move(supported);

    // 当前活动的模型状态
    JsonValue active = JsonValue::makeArray();
    int totalConfigured = 0;
    int totalLoaded = 0;
    int totalFailed = 0;
    int totalMissing = 0;

    if (ctx.pipe) {
        std::vector<ModelSnapshot> models = ctx.pipe->getActiveModels();
        totalConfigured = static_cast<int>(models.size());
        for (const auto& mSnap : models) {
            if (mSnap.status == "loaded") totalLoaded++;
            else if (mSnap.status == "failed") totalFailed++;
            else if (mSnap.status == "missing") totalMissing++;

            JsonValue am = JsonValue::makeObject();
            am.o["id"] = JsonValue::makeString(mSnap.id);
            am.o["displayName"] = JsonValue::makeString(mSnap.displayName);
            am.o["taskType"] = JsonValue::makeString(mSnap.taskType);
            am.o["backend"] = JsonValue::makeString(mSnap.backend);
            if (!mSnap.hash.empty()) am.o["hash"] = JsonValue::makeString(mSnap.hash);
            am.o["status"] = JsonValue::makeString(mSnap.status);
            am.o["isInUse"] = JsonValue::makeBool(mSnap.isInUse);
            if (!mSnap.modelVersion.empty()) am.o["modelVersion"] = JsonValue::makeString(mSnap.modelVersion);
            if (!mSnap.lastError.empty()) am.o["lastError"] = JsonValue::makeString(mSnap.lastError);
            active.a.push_back(std::move(am));
        }
    }
    d.o["activeModels"] = std::move(active);

    JsonValue summary = JsonValue::makeObject();
    summary.o["totalSupported"] = JsonValue::makeNumber(2);
    summary.o["totalConfigured"] = JsonValue::makeNumber(totalConfigured);
    summary.o["totalLoaded"] = JsonValue::makeNumber(totalLoaded);
    summary.o["totalFailed"] = JsonValue::makeNumber(totalFailed);
    summary.o["totalMissing"] = JsonValue::makeNumber(totalMissing);
    d.o["summary"] = std::move(summary);

    return okJson(std::move(d));
}

HttpResp handleReloadModel(const HttpReq& req, EndpointContext& ctx) {
    JsonValue body;
    std::string perr;
    if (!parseJson(req.body, body, perr) || !body.isObject()) {
        return errJson(400, "invalid_json", "请求体需为 JSON object");
    }
    const std::string* id = nullptr;
    if (const JsonValue* idv = body.find("id"); idv && idv->isString()) {
        id = &idv->s;
    }
    if (!id || id->empty()) {
        return errJson(400, "missing_id", "缺少 id 字段");
    }
    std::string status = "not_found";
    if (ctx.pipe) {
        auto models = ctx.pipe->getActiveModels();
        for (const auto& m : models) {
            if (m.id == *id) {
                status = "reload_requested";
                break;
            }
        }
    }
    JsonValue o = JsonValue::makeObject();
    o.o["id"] = JsonValue::makeString(*id);
    o.o["status"] = JsonValue::makeString(status);
    return okJson(std::move(o));
}

// ──── settings ────────────────────────────────────────────────

HttpResp handleGetSettings(const HttpReq&, EndpointContext& ctx) {
    if (!ctx.settings) return errJson(503, "settings_unavailable", "settings 存储未初始化");
    JsonValue doc;
    std::string perr;
    if (!parseJson(ctx.settings->currentRedactedJsonPretty(), doc, perr)) {
        return errJson(500, "settings_parse_error", "设置数据解析失败", {perr});
    }
    return okJson(std::move(doc));
}

HttpResp handlePutSettings(const HttpReq& req, EndpointContext& ctx) {
    if (!ctx.settings) return errJson(503, "settings_unavailable", "settings 存储未初始化");
    const auto res = ctx.settings->updateFromJsonBody(req.body);
    if (!res.ok) return errJson(res.httpStatus, res.code.empty() ? "invalid_request" : res.code, res.message, res.details);
    JsonValue doc;
    std::string perr;
    if (!parseJson(ctx.settings->currentRedactedJsonPretty(), doc, perr)) {
        return errJson(500, "settings_parse_error", "设置数据解析失败", {perr});
    }
    return okJson(std::move(doc));
}

// ──── cameras ─────────────────────────────────────────────────

HttpResp handleGetCameras(const HttpReq&, EndpointContext& ctx) {
    if (!ctx.pipe) return errJson(503, "pipeline_unavailable", "Pipeline 不可用");
    const auto devs = ctx.pipe->devices();
    JsonValue devices = JsonValue::makeArray();
    for (size_t i = 0; i < devs.size(); i++) {
        JsonValue d = JsonValue::makeObject();
        d.o["index"] = JsonValue::makeNumber(static_cast<double>(i));
        d.o["name"] = JsonValue::makeString(utf8FromWide(devs[i].name));
        d.o["deviceId"] = JsonValue::makeString(utf8FromWide(devs[i].deviceId));
        JsonValue formats = JsonValue::makeArray();
        for (const auto& f : devs[i].formats) {
            JsonValue fmt = JsonValue::makeObject();
            fmt.o["w"] = JsonValue::makeNumber(static_cast<double>(f.width));
            fmt.o["h"] = JsonValue::makeNumber(static_cast<double>(f.height));
            fmt.o["fps"] = JsonValue::makeNumber(static_cast<double>(f.fps));
            formats.a.push_back(std::move(fmt));
        }
        d.o["formats"] = std::move(formats);
        devices.a.push_back(std::move(d));
    }
    JsonValue data = JsonValue::makeObject();
    data.o["devices"] = std::move(devices);
    return okJson(std::move(data));
}

HttpResp handleFlipCamera(const HttpReq& req, EndpointContext& ctx) {
    if (!ctx.pipe) return errJson(503, "pipeline_unavailable", "Pipeline 不可用");
    JsonValue doc;
    std::string perr;
    if (!parseJson(req.body, doc, perr) || !doc.isObject()) return errJson(400, "invalid_request", "JSON 解析失败", {perr});
    bool flipX = false;
    bool flipY = false;
    if (const JsonValue* v = doc.find("flipX"); v && v->isBool()) flipX = v->b;
    if (const JsonValue* v = doc.find("flipY"); v && v->isBool()) flipY = v->b;
    ctx.pipe->setFlip(flipX, flipY);
    return okJson(JsonValue::makeObject());
}

// ──── actions ─────────────────────────────────────────────────

HttpResp handleEnroll(const HttpReq& req, EndpointContext& ctx) {
    if (!ctx.pipe) return errJson(503, "pipeline_unavailable", "Pipeline 不可用");
    JsonValue doc;
    std::string perr;
    if (!parseJson(req.body, doc, perr) || !doc.isObject()) return errJson(400, "invalid_request", "JSON 解析失败", {perr});
    std::string personId;
    if (const JsonValue* v = doc.find("personId"); v && v->isString()) personId = v->s;
    personId = trim(personId);
    if (personId.empty()) return errJson(400, "invalid_request", "personId 不能为空");
    ctx.pipe->requestEnroll(personId);
    return okJson(JsonValue::makeObject());
}

HttpResp handleClearDb(const HttpReq&, EndpointContext& ctx) {
    if (!ctx.pipe) return errJson(503, "pipeline_unavailable", "Pipeline 不可用");
    ctx.pipe->requestClearDb();
    return okJson(JsonValue::makeObject());
}

HttpResp handleGetModelsStatus(const HttpReq&, EndpointContext& ctx) {
    if (!ctx.pipe) return errJson(503, "pipeline_unavailable", "Pipeline 不可用");
    auto models = ctx.pipe->getActiveModels();
    int total = 0, loaded = 0, failed = 0, missing = 0;
    for (const auto& m : models) {
        if (m.status == "loaded") loaded++;
        else if (m.status == "failed") failed++;
        else if (m.status == "missing") missing++;
        total++;
    }
    JsonValue summary = JsonValue::makeObject();
    summary.o["totalConfigured"] = JsonValue::makeNumber(total);
    summary.o["totalLoaded"] = JsonValue::makeNumber(loaded);
    summary.o["totalFailed"] = JsonValue::makeNumber(failed);
    summary.o["totalMissing"] = JsonValue::makeNumber(missing);
    return okJson(std::move(summary));
}

// ──── 路由注册 ────────────────────────────────────────────────

void registerAllHttpApi(EndpointRegistry& reg) {
    reg.add({"GET",  "/api/v1/models",          handleGetModels});
    reg.add({"POST", "/api/v1/models/reload",    handleReloadModel});
    reg.add({"GET",  "/api/v1/settings",         handleGetSettings});
    reg.add({"PUT",  "/api/v1/settings",         handlePutSettings});
    reg.add({"GET",  "/api/v1/cameras",          handleGetCameras});
    reg.add({"PUT",  "/api/v1/camera/flip",      handleFlipCamera});
    reg.add({"POST", "/api/v1/actions/enroll",    handleEnroll});
    reg.add({"POST", "/api/v1/actions/db/clear",  handleClearDb});
}

}  // namespace handlers
}  // namespace rk_win
