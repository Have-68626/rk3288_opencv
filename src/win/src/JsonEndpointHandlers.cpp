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
    // 重载后 runtimeReloadGen 递增并随响应返回。
    // ⚠️ 如实口径：generation 的递增是**无条件**执行的（FramePipeline::reloadRuntime 末尾
    // 的 fetch_add 不判成败）⇒ 「generation 变了」只证明重载确实执行过，**不证明装配成功**；
    // 装配成败看下方新增的 ok 键。
    if (!ctx.settings) return errJson(503, "settings_unavailable", "settings 存储未初始化");
    ctx.pipe->reloadRuntime(ctx.settings->current());

    JsonValue o = JsonValue::makeObject();
    o.o["id"] = JsonValue::makeString(*id);
    // status 恒为 "reloaded"（**契约冻结**）：其含义是「存在性校验通过且已执行重载」，
    // 不是「装配成功」。把恒值改为受成败影响属**破坏性契约变更**（既有消费者按字面值判断），
    // 故本批只**新增**下方的 ok / warning 两键承载成败，不改 status 的取值域。
    o.o["status"] = JsonValue::makeString("reloaded");
    o.o["generation"] = JsonValue::makeNumber(static_cast<double>(ctx.pipe->runtimeReloadGen()));
    // 新增（只增不改）：重载后的**装配**成败。修复前重载失败与成功在响应上完全无差别，
    // 调用方无法感知识别器其实没装配起来（FACE-ARCWIN-003 的缺口）。
    // ⚠️ data.ok 与 Envelope 根部的 ok **语义不同**：根部 ok = 「HTTP 业务调用成功」，
    //    本键 = 「重载后运行时装配成功」。warning 仅在降级且文本非空时出现。
    const FramePipeline::DegradedState deg = ctx.pipe->degraded();
    o.o["ok"] = JsonValue::makeBool(!deg.active);
    if (deg.active && !deg.warning.empty()) o.o["warning"] = JsonValue::makeString(deg.warning);
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

    // FACE-CONFIG-001「独立可见通道」：把启动期配置装载结果暴露在 health 上。
    // 为何放在 health：
    //  - 该端点位于匿名白名单（HttpFacesServer::isAnonymousPath 显式放行 health/acceleration），
    //    免 Bearer token 即可读取，监控无需持有凭据；
    //  - 启动流程中 initialize() 先于 http.start() 完成（win_local_service_main.cpp:115 vs :143），
    //    故即便 config.json 被拒、http.port 回落默认值，health 仍能读到诊断状态；
    //  - 「能连上 health」只取决于端口回落，与配置是否被拒无关 —— 这正是本通道成立的前提。
    // 字段出现策略：configStatus 恒出现，使监控能在「配置正常（ok）/降级（degraded）/被拒（rejected）」
    // 「状态未知（unknown，诊断通道缺失）」与「对端是旧版本、尚未实现本字段」之间区分；configWarning /
    // configQuarantinedTo 仅在确有值时才出现，避免噪声。
    // ⚠️ 本节字段全部来自**配置层**（WinJsonConfigStore）；**运行时装配**侧的 status 等字段见
    // 本函数末尾的独立块 —— 二者是不同量，不得合并或互相覆盖。
    // 只读：本端点自身不修改任何状态、不引入锁或缓存；但读取 FramePipeline / Store 的访问器时
    // 被调方**各自持自己的锁**（FramePipeline::degraded() / runtimeReady()），故本端点并非无锁，
    // 只是不持锁跨越调用。见下方 status 字段块。
    // ctx.settings 在生产（win_local_service_main）恒非空；但测试路径可传 nullptr（见
    // test_http_faces_server.cpp 的 runHttpRequest(..., nullptr)），故必须判空兜底（回落
    // unknown）—— 空指针解引用会砸掉整个服务。ctx.pipe 同理须判空（测试路径亦传 nullptr）。
    // configStatus 四态（FACE-CONFIG-011）：ok=装载成功且本次启动无告警；degraded=装载完成但本次启动
    //   存在告警（默认配置写盘失败 / 序列化未通过 schema 等，即 initWarning 非空）；rejected=启动期配置
    //   被拒（已改名另存并以默认值启动）；unknown=无法判定（ctx.settings 为空，见上）。
    //   判据优先级：unknown > rejected > degraded > ok。
    //   ⚠️ degraded 只在「非 rejected 且 initWarning 非空」时成立：已逐条核实 initialize 中所有给 warnOut
    //   赋值的路径——(1) 三条落在「既有配置无法装载」的 rejected 分支（被 rejected 优先吞掉）；(2) 默认
    //   配置落盘失败；(3) 序列化未通过 schema（密钥不可用）而拒写。后两类均为真实降级，**无良性提示**，
    //   故不会把正常情况误判为 degraded。
    // 未知不可当正常 —— "ok" 是「配置已校验通过且无告警」的正向信号，若诊断通道缺失时回落 "ok"，
    // 配置被拒/降级会在监控侧静默通过，失去本通道的意义。
    const char* configStatus = "unknown";
    if (ctx.settings) {
        if (ctx.settings->configRejectedOnLoad()) {
            configStatus = "rejected";
        } else if (!ctx.settings->initWarning().empty()) {
            configStatus = "degraded";
        } else {
            configStatus = "ok";
        }
    }
    d.o["configStatus"] = JsonValue::makeString(configStatus);
    if (ctx.settings) {
        const std::string warning = ctx.settings->initWarning();
        if (!warning.empty()) {
            d.o["configWarning"] = JsonValue::makeString(warning);
        }
        const std::string quarantined = ctx.settings->initQuarantineFileName();
        if (!quarantined.empty()) {
            d.o["configQuarantinedTo"] = JsonValue::makeString(quarantined);
        }
    }

    // ── 运行时**装配**状态（FACE-ARCWIN-003 可观测性部分）───────────────────
    // 与上面的 configStatus **不是同一个量**，故两者并存、互不覆盖、不合并（勿以为是重复字段）：
    //   configStatus     = 启动期**配置装载**状态：config.json 是否被拒 / 装载是否有告警，
    //                      来源 WinJsonConfigStore（配置层）。
    //   status（本字段）= 运行时**装配**状态：识别器 / DNN 是否装配成功，来源 FramePipeline。
    // 二者可任意组合，例如：config.json 完全合法（configStatus=ok）而模型文件缺失
    // （status=degraded）⇒ 只看 configStatus 会漏报「服务起来了但识别功能缺失」，这正是本字段
    // 要堵的盲区（该告警此前只出现在「设置→模型状态」Tab，且从不经 HTTP 上线）。
    // 三态判据（优先级形态照抄上面 configStatus）：starting > degraded > ok
    //   starting = ctx.pipe 未挂载（测试路径）或 pipeline 尚未装配（未 initialize / reloadRuntime）；
    //   degraded = 装配失败（见 FramePipeline::DegradedState）；
    //   ok       = 已装配且无降级。
    // ⚠️ 本字段**只读**：degraded **绝不映射为任何非 2xx**，也不阻断任何操作或端点
    //    ——「自检失败是否 fail-closed」属未拍板的产品决策，此处不得提前施加阻断语义。
    // 只读访问器自身的线程安全由 FramePipeline::degraded() / runtimeReady() 保证（各自持锁）。
    const bool runtimeAssembled = ctx.pipe && ctx.pipe->runtimeReady();
    const FramePipeline::DegradedState deg =
        ctx.pipe ? ctx.pipe->degraded() : FramePipeline::DegradedState{};
    const char* runtimeStatus = !runtimeAssembled ? "starting" : (deg.active ? "degraded" : "ok");
    d.o["status"] = JsonValue::makeString(runtimeStatus);
    // availableFeatures 恒出现（三态下皆有意义，未知/降级时可能为空数组）；恒出现的理由与
    // configStatus 相同：使监控能区分「本版本未实现该字段」与「当前确实没有可用模型」。
    JsonValue features = JsonValue::makeArray();
    for (const auto& id : deg.availableFeatures) features.a.push_back(JsonValue::makeString(id));
    d.o["availableFeatures"] = std::move(features);
    // 按需字段：仅在确实降级时出现（与 configWarning 同策——确有值才出现，避免噪声）。
    // `warning` 是服务端告警原文，**可能含文件系统路径**（与 configWarning 同类，按本机可见对待）。
    // `degradedSince` 是**首次**进入降级的时间（`%Y-%m-%dT%H:%M:%S.mmm` 本地时间、无时区后缀）。
    if (runtimeAssembled && deg.active) {
        if (!deg.reasonCode.empty()) d.o["reasonCode"] = JsonValue::makeString(deg.reasonCode);
        if (!deg.warning.empty()) d.o["warning"] = JsonValue::makeString(deg.warning);
        if (!deg.degradedSinceIso.empty()) {
            d.o["degradedSince"] = JsonValue::makeString(deg.degradedSinceIso);
        }
    }
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
    // 为文档镜像。两者的 `METHOD path` 集合与逐条 summary 由 tests/scripts/openapi-mirror.test.js
    // 机械比对（本注释此前声称「S3 三方校验要求两者 paths 集合一致」，而仓内**并无**该实现——
    // 该措辞已由上述测试取代）。新增/变更端点时须同时更新本字符串与 openapi.yaml。
    // 流式端点（/api/faces/stream、/api/v1/faces/stream、/api/v1/preview.mjpeg）由
    // StreamSessionRunner 旁路处理（HttpFacesServer::run 经 stream::isStreamRequest 分流，
    // 见 StreamSession.cpp），不在 EndpointRegistry 注册表中，但此处同样声明以保持文档完整。
    static const char* kOpenApiJson = R"JSON({"openapi":"3.0.3","info":{"title":"rk_wcfr Local API","version":"1.0","description":"RK_WCFR（Windows Camera Face Recognition）本地 REST API。"},"paths":{"/api/v1/health":{"get":{"summary":"健康检查","responses":{"200":{"description":"OK"}}}},"/api/v1/acceleration":{"get":{"summary":"加速配置与活动后端","responses":{"200":{"description":"OK"}}}},"/api/v1/openapi":{"get":{"summary":"获取本 OpenAPI 规范（JSON）","responses":{"200":{"description":"OpenAPI 规范 JSON"}}}},"/openapi.json":{"get":{"summary":"获取本 OpenAPI 规范（JSON，别名路径）","responses":{"200":{"description":"OpenAPI 规范 JSON"}}}},"/api/v1/models":{"get":{"summary":"模型列表与状态","responses":{"200":{"description":"OK"}}}},"/api/v1/models/reload":{"post":{"summary":"重载运行时（按 id 作存在性校验；整体重建，不按 id 选择对象）","responses":{"200":{"description":"OK"}}}},"/api/v1/settings":{"get":{"summary":"获取完整配置（敏感字段已脱敏）","responses":{"200":{"description":"OK"}}},"put":{"summary":"更新配置（局部更新；敏感字段加密落盘）","responses":{"200":{"description":"OK"}}}},"/api/v1/cameras":{"get":{"summary":"摄像头设备列表","responses":{"200":{"description":"OK"}}}},"/api/v1/camera/flip":{"put":{"summary":"设置画面翻转","responses":{"200":{"description":"OK"}}}},"/api/faces":{"get":{"summary":"当前帧人脸快照","responses":{"200":{"description":"OK"}}}},"/api/v1/faces":{"get":{"summary":"当前帧人脸快照（版本化路径）","responses":{"200":{"description":"OK"}}}},"/api/v1/actions/enroll":{"post":{"summary":"注册人脸","responses":{"200":{"description":"OK"}}}},"/api/v1/actions/db/clear":{"post":{"summary":"清空人脸库","responses":{"200":{"description":"OK"}}}},"/api/v1/actions/crypto/rotate":{"post":{"summary":"轮换加密主密钥","responses":{"200":{"description":"OK"}}}},"/api/v1/actions/privacy/open":{"post":{"summary":"打开隐私设置","responses":{"200":{"description":"OK"}}}},"/api/v1/preview.jpg":{"get":{"summary":"实时预览 JPEG 帧","responses":{"200":{"description":"image/jpeg"}}}},"/api/faces/stream":{"get":{"summary":"人脸识别结果 SSE 流（StreamSessionRunner 旁路）","responses":{"200":{"description":"text/event-stream"}}}},"/api/v1/faces/stream":{"get":{"summary":"人脸识别结果 SSE 流（版本化路径，StreamSessionRunner 旁路）","responses":{"200":{"description":"text/event-stream"}}}},"/api/v1/preview.mjpeg":{"get":{"summary":"实时预览 MJPEG 流（StreamSessionRunner 旁路）","responses":{"200":{"description":"multipart/x-mixed-replace"}}}}}})JSON";
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
