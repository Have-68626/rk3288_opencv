#pragma once
#include "rk_win/HttpFacesServer.h"

#include <functional>
#include <string>
#include <vector>

namespace rk_win {

class FramePipeline;
class WinJsonConfigStore;

/// 端点 dispatch 上下文：挂载 pipeline / settings 等运行时依赖
struct EndpointContext {
    FramePipeline* pipe = nullptr;
    WinJsonConfigStore* settings = nullptr;
};

/// 路由定义：method/path + handler
struct EndpointDef {
    const char* method;   // "GET", "POST", "PUT", "*" (any)
    const char* path;
    std::function<HttpFacesServer::HttpResponse(const HttpFacesServer::HttpRequest&, EndpointContext&)> handler;
};

/// 路由注册表：替代 HttpFacesServer 中 kRoutes + handleApi 的原始 if-else 模式
class EndpointRegistry {
public:
    void add(EndpointDef def);
    HttpFacesServer::HttpResponse dispatch(const HttpFacesServer::HttpRequest& req, EndpointContext& ctx) const;

private:
    std::vector<EndpointDef> routes_;
};

/// 统一响应工厂：生成标准 JSON 响应
namespace ResponseFactory {
    HttpFacesServer::HttpResponse ok(const std::string& jsonBody = "");
    HttpFacesServer::HttpResponse err(int code, const std::string& msg, const std::string& details = "");
}

}  // namespace rk_win
