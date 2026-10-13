# CivetWeb SSL 验证归档说明

**快照事实**: `src/win/third_party/civetweb/civetweb.c:18341` 使用 `SSL_VERIFY_NONE`。
**当前适用性**: CivetWeb 1.16 未编入任何构建目标，Windows 本地服务使用自有 Winsock `HttpFacesServer`；该位置不属于当前运行路径，因而不是现行产品风险。
**未来接入要求**: 若重新将 CivetWeb 编入目标并启用其 HTTPS 客户端功能，接入前必须将验证策略改为 `SSL_VERIFY_PEER`，并新增证书校验失败、受信任链和主机名不匹配的自动化测试。
