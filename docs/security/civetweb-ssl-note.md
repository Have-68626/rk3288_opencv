# civetweb SSL 验证说明

**问题**: `civetweb.c:18341` 使用 `SSL_VERIFY_NONE`
**风险**: 中等 — 该代码在 civetweb 作为 HTTPS 客户端时跳过证书验证
**缓解**: 项目当前仅将 civetweb 用于服务器端 HTTP 服务，未启用客户端功能
**建议**: 如果将来启用 civetweb 客户端功能，需将 `SSL_VERIFY_NONE` 改为 `SSL_VERIFY_PEER`
