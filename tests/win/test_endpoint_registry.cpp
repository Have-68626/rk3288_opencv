#include "rk_win/EndpointRegistry.h"
#include "rk_win/FramePipeline.h"
#include "rk_win/JsonEndpointHandlers.h"
#include "rk_win/JsonLite.h"
#include "rk_win/WinJsonConfig.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

// ──── 测试：精确匹配返回 200 ────────────────────────────────
bool test_registry_dispatch_returns_200_for_matching_route() {
    rk_win::EndpointRegistry reg;
    reg.add({
        "GET",
        "/test",
        [](const rk_win::HttpFacesServer::HttpRequest&, rk_win::EndpointContext&)
            -> rk_win::HttpFacesServer::HttpResponse {
            rk_win::HttpFacesServer::HttpResponse resp;
            resp.status = 200;
            resp.reason = "OK";
            resp.contentType = "application/json; charset=utf-8";
            resp.body = "{\"ok\":true}";
            resp.close = true;
            return resp;
        }
    });

    rk_win::HttpFacesServer::HttpRequest req;
    req.method = "GET";
    req.path = "/test";

    rk_win::EndpointContext ctx;
    const auto resp = reg.dispatch(req, ctx);

    if (resp.status != 200) {
        std::cerr << "Expected status 200, got " << resp.status << std::endl;
        return false;
    }
    if (resp.body != "{\"ok\":true}") {
        std::cerr << "Expected body {\"ok\":true}, got " << resp.body << std::endl;
        return false;
    }
    return true;
}

// ──── 测试：Method 不匹配返回 405 ────────────────────────────
bool test_registry_returns_405_for_method_mismatch() {
    rk_win::EndpointRegistry reg;
    reg.add({
        "POST",
        "/test",
        [](const rk_win::HttpFacesServer::HttpRequest&, rk_win::EndpointContext&)
            -> rk_win::HttpFacesServer::HttpResponse {
            rk_win::HttpFacesServer::HttpResponse resp;
            resp.status = 200;
            resp.reason = "OK";
            resp.contentType = "application/json; charset=utf-8";
            resp.body = "{\"ok\":true}";
            resp.close = true;
            return resp;
        }
    });

    rk_win::HttpFacesServer::HttpRequest req;
    req.method = "GET";
    req.path = "/test";

    rk_win::EndpointContext ctx;
    const auto resp = reg.dispatch(req, ctx);

    if (resp.status != 405) {
        std::cerr << "Expected status 405, got " << resp.status << std::endl;
        return false;
    }
    return true;
}

namespace {
// RK_WCFR_CONFIG 环境变量注入临时 config.json 路径（WinJsonConfigStore::defaultConfigPath 读取）。
// 析构时删除环境变量，避免污染同进程其他测试的 WinJsonConfigStore 构造。
struct ConfigEnvGuard {
    explicit ConfigEnvGuard(const std::filesystem::path& cfgPath) {
        SetEnvironmentVariableW(L"RK_WCFR_CONFIG", cfgPath.wstring().c_str());
    }
    ~ConfigEnvGuard() { SetEnvironmentVariableW(L"RK_WCFR_CONFIG", NULL); }
};

std::filesystem::path makeTempDir(const std::string& suffix) {
    const auto base = std::filesystem::temp_directory_path() / "rk3288_opencv_win_endpoint_tests";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    const auto dir = base / suffix;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}
}  // namespace

// B06-020：POST /api/v1/models/reload 必须真正触发运行时重载，成功后代次必须变化。
// 修复前：handleReloadModel 仅返回 status="reload_requested"，从不调用 reloadRuntime，
//   runtimeReloadGen 恒不变 → gen1 == gen0，断言失败（RED）。
// 修复后：端点以当前配置调用 pipe->reloadRuntime，代次 +1，且响应携带新代次 → GREEN。
bool test_reload_endpoint_bumps_generation() {
    const auto dir = makeTempDir("reload_gen");
    ConfigEnvGuard env(dir / "config.json");

    rk_win::WinJsonConfigStore store;
    std::string warn;
    if (!store.initialize(warn)) return false;

    // 不调 initialize（避免枚举真实相机/启动线程），直接 reloadRuntime 填充 activeModels_
    rk_win::FramePipeline fp;
    rk_win::AppConfig cfg;
    cfg.model.recognition = "lbph";
    cfg.recognition.cascadePath = "assets/lbpcascade_frontalface.xml";
    cfg.recognition.databasePath = ".";
    cfg.dnn.enable = false;
    fp.reloadRuntime(cfg);

    rk_win::EndpointRegistry reg;
    rk_win::handlers::registerAllHttpApi(reg);

    rk_win::EndpointContext ctx;
    ctx.pipe = &fp;
    ctx.settings = &store;

    const std::uint64_t gen0 = fp.runtimeReloadGen();

    rk_win::HttpFacesServer::HttpRequest req;
    req.method = "POST";
    req.path = "/api/v1/models/reload";
    req.body = "{\"id\":\"cascade_frontalface\"}";
    const auto resp = reg.dispatch(req, ctx);

    const std::uint64_t gen1 = fp.runtimeReloadGen();
    fp.shutdown();

    // 成功重载必须改变代次（B06-020 治理项：禁止"伪成功"）
    if (resp.status != 200 || gen1 <= gen0) {
        std::cerr << "reload 后 generation 未变化: gen0=" << gen0 << " gen1=" << gen1
                  << " http=" << resp.status << std::endl;
        return false;
    }
    // 响应须携带新的 generation，且与管线代次一致
    rk_win::JsonValue bodyDoc;
    std::string perr;
    if (!rk_win::parseJson(resp.body, bodyDoc, perr) || !bodyDoc.isObject()) return false;
    const rk_win::JsonValue* okv = bodyDoc.find("ok");
    if (!okv || !okv->isBool() || !okv->b) return false;
    const rk_win::JsonValue* data = bodyDoc.find("data");
    if (!data || !data->isObject()) return false;
    const rk_win::JsonValue* genv = data->find("generation");
    return genv && genv->isNumber() && genv->n == static_cast<double>(gen1);
}

// B08-005：openapi 端点必须返回含 openapi/info/paths 的规范 JSON，而非占位响应。
// 修复前：handleGetOpenApi 仅返回 {"ok":true,"data":{"note":"..."}} 占位，
//   顶层无 openapi/info/paths 键 → 断言失败（RED）。
// 修复后：返回 OpenAPI 3.0.3 规范文档（openapi/info/paths 齐备）→ 断言通过（GREEN）。
bool test_openapi_endpoint_returns_spec_json() {
    rk_win::EndpointRegistry reg;
    rk_win::handlers::registerAllHttpApi(reg);
    rk_win::EndpointContext ctx;

    const char* pathsToCheck[] = {"/api/v1/openapi", "/openapi.json"};
    for (const char* p : pathsToCheck) {
        rk_win::HttpFacesServer::HttpRequest req;
        req.method = "GET";
        req.path = p;
        const auto resp = reg.dispatch(req, ctx);
        if (resp.status != 200) return false;

        rk_win::JsonValue doc;
        std::string perr;
        if (!rk_win::parseJson(resp.body, doc, perr) || !doc.isObject()) return false;
        const rk_win::JsonValue* ver = doc.find("openapi");
        const rk_win::JsonValue* info = doc.find("info");
        const rk_win::JsonValue* paths = doc.find("paths");
        if (!ver || !ver->isString() || ver->s != "3.0.3") {
            std::cerr << "openapi 响应缺少 openapi 键或版本不正确: path=" << p << std::endl;
            return false;
        }
        if (!info || !info->isObject() || !info->find("title")) {
            std::cerr << "openapi 响应缺少 info 键: path=" << p << std::endl;
            return false;
        }
        if (!paths || !paths->isObject()) {
            std::cerr << "openapi 响应缺少 paths 键: path=" << p << std::endl;
            return false;
        }
    }
    return true;
}
