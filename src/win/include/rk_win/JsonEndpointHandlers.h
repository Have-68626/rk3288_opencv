#pragma once
#include "rk_win/EndpointRegistry.h"

namespace rk_win {
namespace handlers {

// models 端点
HttpFacesServer::HttpResponse handleGetModels(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);
HttpFacesServer::HttpResponse handleReloadModel(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);

// settings 端点
HttpFacesServer::HttpResponse handleGetSettings(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);
HttpFacesServer::HttpResponse handlePutSettings(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);

// cameras 端点
HttpFacesServer::HttpResponse handleGetCameras(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);
HttpFacesServer::HttpResponse handleFlipCamera(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);

// actions 端点
HttpFacesServer::HttpResponse handleEnroll(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);
HttpFacesServer::HttpResponse handleClearDb(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);
HttpFacesServer::HttpResponse handleCryptoRotate(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);
HttpFacesServer::HttpResponse handlePrivacyOpen(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);
HttpFacesServer::HttpResponse handlePreviewJpg(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);

// 基础端点
HttpFacesServer::HttpResponse handleGetHealth(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);
HttpFacesServer::HttpResponse handleGetAcceleration(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);
HttpFacesServer::HttpResponse handleGetOpenApi(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);
HttpFacesServer::HttpResponse handleGetFaces(
    const HttpFacesServer::HttpRequest& req, EndpointContext& ctx);

// 填充路由表
void registerAllHttpApi(EndpointRegistry& reg);

}  // namespace handlers
}  // namespace rk_win
