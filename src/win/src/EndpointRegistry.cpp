#include "rk_win/EndpointRegistry.h"

namespace rk_win {

void EndpointRegistry::add(EndpointDef def) {
    routes_.push_back(std::move(def));
}

HttpFacesServer::HttpResponse EndpointRegistry::dispatch(const HttpFacesServer::HttpRequest& req, EndpointContext& ctx) const {
    for (const auto& r : routes_) {
        if (req.path == r.path) {
            if (r.method[0] != '*' && req.method != r.method) {
                return ResponseFactory::err(405, "method_not_allowed",
                                            "仅支持 " + std::string(r.method));
            }
            return r.handler(req, ctx);
        }
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
