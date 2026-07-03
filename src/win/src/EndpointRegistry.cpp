#include "rk_win/EndpointRegistry.h"

namespace rk_win {

void EndpointRegistry::add(EndpointDef def) {
    routes_.push_back(std::move(def));
}

HttpFacesServer::HttpResponse EndpointRegistry::dispatch(const HttpFacesServer::HttpRequest& req, EndpointContext& ctx) const {
    bool pathMatched = false;
    for (const auto& r : routes_) {
        if (req.path == r.path) {
            pathMatched = true;
            if (r.method[0] != '*' && req.method != r.method) {
                continue;  // 方法不匹配，继续查找同路径的其他路由
            }
            return r.handler(req, ctx);
        }
    }
    if (pathMatched) {
        return ResponseFactory::err(405, "method_not_allowed", "不支持的请求方法");
    }
    return ResponseFactory::err(404, "not_found", "未知端点");
}

namespace ResponseFactory {

HttpFacesServer::HttpResponse ok(const std::string& jsonBody) {
    HttpFacesServer::HttpResponse resp;
    resp.status = 200;
    resp.reason = "OK";
    resp.contentType = "application/json; charset=utf-8";
    resp.body = jsonBody.empty() ? "{}" : jsonBody;
    resp.close = true;
    return resp;
}

HttpFacesServer::HttpResponse err(int code, const std::string& msg, const std::string& details) {
    HttpFacesServer::HttpResponse resp;
    resp.status = code;
    resp.reason = (code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" : "Error");
    resp.contentType = "application/json; charset=utf-8";
    resp.body = "{\"error\":\"" + msg + "\",\"details\":\"" + details + "\"}";
    resp.close = true;
    return resp;
}

}  // namespace ResponseFactory
}  // namespace rk_win
