#include "rk_win/EndpointRegistry.h"

#include <iostream>
#include <string>

namespace {

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

}  // namespace
