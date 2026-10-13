#include "rk_win/JsonEndpointHandlers.h"
#include "rk_win/FacesJson.h"
#include "rk_win/FramePipeline.h"
#include "rk_win/JsonLite.h"
#include "rk_win/WinJsonConfig.h"

#include <cctype>
#include <string>
#include <vector>

#if __has_include(<opencv2/imgcodecs.hpp>)
#define RK_HANDLERS_HAS_OPENCV 1
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#else
#define RK_HANDLERS_HAS_OPENCV 0
#endif

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
    // 目标模型须存在于当前活动模型清单中；不存在则返回 not_found（不触发重载）
    bool found = false;
    if (ctx.pipe) {
        auto models = ctx.pipe->getActiveModels();
        for (const auto& m : models) {
            if (m.id == *id) {
                found = true;
                break;
            }
        }
    }
    if (!found) {
        JsonValue o = JsonValue::makeObject();
        o.o["id"] = JsonValue::makeString(*id);
        o.o["status"] = JsonValue::makeString("not_found");
        return okJson(std::move(o));
    }

    // 真正执行重载（B06-020 治理项）：修复前仅返回 status="reload_requested" 占位、
    // 从不调用 reloadRuntime，属于"伪成功"。现以当前生效配置原子重建运行时快照，
    // 成功后 runtimeReloadGen 递增并随响应返回——成功必改变 generation。
    if (!ctx.settings) return errJson(503, "settings_unavailable", "settings 存储未初始化");
    ctx.pipe->reloadRuntime(ctx.settings->current());

    JsonValue o = JsonValue::makeObject();
    o.o["id"] = JsonValue::makeString(*id);
    o.o["status"] = JsonValue::makeString("reloaded");
    o.o["generation"] = JsonValue::makeNumber(static_cast<double>(ctx.pipe->runtimeReloadGen()));
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

// ──── crypto / privacy / preview ─────────────────────────────────

HttpResp handleCryptoRotate(const HttpReq&, EndpointContext& ctx) {
    if (!ctx.settings) return errJson(503, "settings_unavailable", "settings 存储未初始化");
    const auto res = ctx.settings->rotateKeyAndReencrypt();
    if (!res.ok) return errJson(res.httpStatus, res.code.empty() ? "internal_error" : res.code, res.message, res.details);
    return okJson(JsonValue::makeObject());
}

HttpResp handlePrivacyOpen(const HttpReq&, EndpointContext& ctx) {
    if (!ctx.pipe) return errJson(503, "pipeline_unavailable", "Pipeline 不可用");
    ctx.pipe->openPrivacySettings();
    return okJson(JsonValue::makeObject());
}

HttpResp handlePreviewJpg(const HttpReq&, EndpointContext& ctx) {
#if RK_HANDLERS_HAS_OPENCV
    if (!ctx.pipe) return errJson(503, "pipeline_unavailable", "Pipeline 不可用");
    RenderState rs;
    if (!ctx.pipe->tryGetRenderState(rs)) return errJson(503, "frame_unavailable", "暂无可用画面");

    cv::Mat img = rs.bgr;
    if (img.empty()) return errJson(503, "frame_unavailable", "暂无可用画面");

    for (const auto& f : rs.faces) {
        cv::rectangle(img, f.rect, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
    }
    static const std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, 80};
    std::vector<uchar> buf;
    if (!cv::imencode(".jpg", img, buf, jpegParams)) return errJson(500, "encode_failed", "JPEG 编码失败");

    HttpResp r;
    r.status = 200;
    r.reason = "OK";
    r.contentType = "image/jpeg";
    r.headers.push_back({"Cache-Control", "no-cache"});
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    r.headers.push_back({"Content-Security-Policy", "default-src 'none'"});
    r.body.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
    return r;
#else
    (void)ctx;
    return errJson(503, "opencv_unavailable", "OpenCV 不可用，无法输出预览");
#endif
}

// ──── health / acceleration / openapi / faces ────────────────────

HttpResp handleGetHealth(const HttpReq&, EndpointContext& ctx) {
    JsonValue d = JsonValue::makeObject();
    d.o["name"] = JsonValue::makeString("rk_wcfr");
    d.o["port"] = JsonValue::makeNumber(static_cast<double>(ctx.port));
    return okJson(std::move(d));
}

HttpResp handleGetAcceleration(const HttpReq&, EndpointContext& ctx) {
    JsonValue d = JsonValue::makeObject();

    if (ctx.settings) {
        std::string redacted = ctx.settings->currentRedactedJsonPretty();
        JsonValue settingsDoc;
        std::string perr;
        if (parseJson(redacted, settingsDoc, perr) && settingsDoc.isObject()) {
            if (const JsonValue* accel = settingsDoc.find("acceleration"); accel && accel->isObject()) {
                d.o["config"] = *accel;
            }
            if (const JsonValue* dnn = settingsDoc.find("dnn"); dnn && dnn->isObject()) {
                JsonValue dnnSummary = JsonValue::makeObject();
                if (const JsonValue* e = dnn->find("enable")) dnnSummary.o["enable"] = *e;
                if (const JsonValue* b = dnn->find("backend")) dnnSummary.o["backend"] = *b;
                if (const JsonValue* bt = dnn->find("confThreshold")) dnnSummary.o["confThreshold"] = *bt;
                d.o["dnn"] = std::move(dnnSummary);
            }
        }
    }

    JsonValue backends = JsonValue::makeArray();
    if (ctx.pipe) {
        auto models = ctx.pipe->getActiveModels();
        for (const auto& m : models) {
            if (m.isInUse) {
                JsonValue b = JsonValue::makeObject();
                b.o["id"] = JsonValue::makeString(m.id);
                b.o["backend"] = JsonValue::makeString(m.backend);
                b.o["status"] = JsonValue::makeString(m.status);
                backends.a.push_back(std::move(b));
            }
        }
    }
    d.o["activeBackends"] = std::move(backends);
    return okJson(std::move(d));
}

HttpResp handleGetOpenApi(const HttpReq&, EndpointContext&) {
    // OpenAPI 3.0.3 规范 JSON（B08-005 治理项）：修复前为占位响应（仅含 note 字段，
    // 无 openapi/info/paths 结构）。此处返回真实规范文档。
    // 注意同步关系：运行时以本内嵌 JSON 为事实源；docs/windows-web-spa/openapi.yaml
    // 为文档镜像（S3 三方校验要求两者 paths 集合一致）。新增/变更端点时须同时更新
    // 本字符串与 openapi.yaml。
    // 流式端点（/api/faces/stream、/api/v1/faces/stream、/api/v1/preview.mjpeg）由
    // StreamSessionRunner 旁路处理（HttpFacesServer::run 经 stream::isStreamRequest 分流，
    // 见 StreamSession.cpp），不在 EndpointRegistry 注册表中，但此处同样声明以保持文档完整。
    static const char* kOpenApiJson = R"JSON({"openapi":"3.0.3","info":{"title":"rk_wcfr Local API","version":"1.0","description":"RK_WCFR（Windows Camera Face Recognition）本地 REST API。"},"paths":{"/api/v1/health":{"get":{"summary":"健康检查","responses":{"200":{"description":"OK"}}}},"/api/v1/acceleration":{"get":{"summary":"加速配置与活动后端","responses":{"200":{"description":"OK"}}}},"/api/v1/openapi":{"get":{"summary":"获取本 OpenAPI 规范（JSON）","responses":{"200":{"description":"OpenAPI 规范 JSON"}}}},"/openapi.json":{"get":{"summary":"获取本 OpenAPI 规范（JSON，别名路径）","responses":{"200":{"description":"OpenAPI 规范 JSON"}}}},"/api/v1/models":{"get":{"summary":"模型列表与状态","responses":{"200":{"description":"OK"}}}},"/api/v1/models/reload":{"post":{"summary":"按 id 重载模型运行时（成功后代次 generation 递增）","responses":{"200":{"description":"OK"}}}},"/api/v1/settings":{"get":{"summary":"获取完整配置（敏感字段已脱敏）","responses":{"200":{"description":"OK"}}},"put":{"summary":"更新配置（局部更新；敏感字段加密落盘）","responses":{"200":{"description":"OK"}}}},"/api/v1/cameras":{"get":{"summary":"摄像头设备列表","responses":{"200":{"description":"OK"}}}},"/api/v1/camera/flip":{"put":{"summary":"设置画面翻转","responses":{"200":{"description":"OK"}}}},"/api/faces":{"get":{"summary":"人脸库快照","responses":{"200":{"description":"OK"}}}},"/api/v1/faces":{"get":{"summary":"人脸库快照（版本化路径）","responses":{"200":{"description":"OK"}}}},"/api/v1/actions/enroll":{"post":{"summary":"注册人脸","responses":{"200":{"description":"OK"}}}},"/api/v1/actions/db/clear":{"post":{"summary":"清空人脸库","responses":{"200":{"description":"OK"}}}},"/api/v1/actions/crypto/rotate":{"post":{"summary":"轮换加密主密钥","responses":{"200":{"description":"OK"}}}},"/api/v1/actions/privacy/open":{"post":{"summary":"打开隐私设置","responses":{"200":{"description":"OK"}}}},"/api/v1/preview.jpg":{"get":{"summary":"实时预览 JPEG 帧","responses":{"200":{"description":"image/jpeg"}}}},"/api/faces/stream":{"get":{"summary":"人脸识别结果 SSE 流（StreamSessionRunner 旁路）","responses":{"200":{"description":"text/event-stream"}}}},"/api/v1/faces/stream":{"get":{"summary":"人脸识别结果 SSE 流（版本化路径，StreamSessionRunner 旁路）","responses":{"200":{"description":"text/event-stream"}}}},"/api/v1/preview.mjpeg":{"get":{"summary":"实时预览 MJPEG 流（StreamSessionRunner 旁路）","responses":{"200":{"description":"multipart/x-mixed-replace"}}}}}})JSON";
    HttpResp r;
    r.status = 200;
    r.reason = "OK";
    r.contentType = "application/json; charset=utf-8";
    r.body = kOpenApiJson;
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    r.headers.push_back({"Content-Security-Policy", "default-src 'none'"});
    return r;
}

HttpResp handleGetFaces(const HttpReq&, EndpointContext& ctx) {
    if (!ctx.pipe) return errJson(503, "pipeline_unavailable", "Pipeline 不可用");
    FacesSnapshot snap;
    if (!ctx.pipe->snapshotFaces(snap)) return errJson(503, "pipeline_unavailable", "Pipeline 不可用");
    HttpResp r;
    r.status = 200;
    r.reason = "OK";
    r.contentType = "application/json; charset=utf-8";
    r.body = buildFacesJson(snap);
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    r.headers.push_back({"Content-Security-Policy", "default-src 'none'"});
    return r;
}

void registerAllHttpApi(EndpointRegistry& reg) {
    // 基础端点
    reg.add({"GET",  "/api/v1/health",             handleGetHealth});
    reg.add({"GET",  "/api/v1/acceleration",       handleGetAcceleration});
    reg.add({"GET",  "/api/v1/openapi",            handleGetOpenApi});
    reg.add({"GET",  "/openapi.json",              handleGetOpenApi});

    // models 端点
    reg.add({"GET",  "/api/v1/models",             handleGetModels});
    reg.add({"POST", "/api/v1/models/reload",       handleReloadModel});

    // settings 端点
    reg.add({"GET",  "/api/v1/settings",            handleGetSettings});
    reg.add({"PUT",  "/api/v1/settings",            handlePutSettings});

    // cameras 端点
    reg.add({"GET",  "/api/v1/cameras",             handleGetCameras});
    reg.add({"PUT",  "/api/v1/camera/flip",         handleFlipCamera});

    // faces 端点
    reg.add({"GET",  "/api/faces",                  handleGetFaces});
    reg.add({"GET",  "/api/v1/faces",               handleGetFaces});

    // actions 端点
    reg.add({"POST", "/api/v1/actions/enroll",       handleEnroll});
    reg.add({"POST", "/api/v1/actions/db/clear",     handleClearDb});
    reg.add({"POST", "/api/v1/actions/crypto/rotate",handleCryptoRotate});
    reg.add({"POST", "/api/v1/actions/privacy/open", handlePrivacyOpen});

    // preview
    reg.add({"GET",  "/api/v1/preview.jpg",          handlePreviewJpg});
}

}  // namespace handlers
}  // namespace rk_win
