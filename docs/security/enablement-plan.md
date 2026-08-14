# 最小启用方案（Security Enablement Plan）

> 治理任务 S-SEC-B10-007（SEC-16）产物。与 `SECURITY.md` 配套，回答：
> 哪些安全功能在默认配置下**开/关**，生产部署如何以最小步骤启用。
> 规格事实源：`docs/security/auth-model.md`、`docs/security/face-db-encryption.md`
> （均为冻结规格）；本清单的"当前状态"已对照 `src/win/src/HttpFacesServer.cpp`、
> `src/win/src/FaceDatabase.cpp` 源码核实（2026-08-11）。

## 1. 默认开/关清单

| 安全功能 | 当前状态 | 是否可关闭 | 说明 |
|---|---|---|---|
| REST API 认证（Bearer token） | **关（默认）** | 是 | token 为空 → `requestAuthorized` 放行（兼容模式）；启用后除匿名白名单外一律 401 |
| CSRF 防护（写方法） | **开（无条件）** | 否 | `csrfCheckPasses` 对 POST/PUT/DELETE 恒校验；读方法恒放行；认证兼容模式下仍生效（纵深正交） |
| 人脸特征库加密（AES-256-GCM） | **开（无条件）** | 否 | `save()` 一律写加密容器；`load()` 自动迁移旧明文 YAML（含 `.pre-encrypt` 回滚点）；密钥缺失时 `save` 失败而非明文回退 |
| 日志敏感字段脱敏 | 开 | 否 | NativeLog 脱敏纪律（S-SEC-B07-001） |
| 日志 URI userinfo 脱敏 | 开 | 否 | `sanitizeUriForLog`（S-SEC-B07-002） |
| JNI/整数溢出防护（32 位 checked arithmetic） | 开 | 否 | S-SEC-B09-003 |
| CivetWeb TLS 证书校验 | 不适用 | — | 未编入构建；重新接入前必须 `SSL_VERIFY_PEER` |

**结论**：需要显式启用的仅**一项**——REST API 认证（设环境变量）。加密与 CSRF
均默认无条件生效，不可关闭。

## 2. 启用建议（生产部署最小清单）

1. **启用认证（唯一手动步骤）**：
   - 在服务启动环境设置强随机值 `RK_WCFR_HTTP_AUTH_TOKEN`（建议 ≥32 字符）。
   - 说明：配置字段 `http.authToken` 未实现（已登记待 CONFIG 任务，见
     `auth-model.md` §2），token 只从环境变量读取。
2. **验证加密与 CSRF 已生效**（无需配置）：
   - 加密：`tests/win/test_face_database_encryption.cpp`（新库无明文特征、旧库可迁移）。
   - CSRF：`tests/win/test_http_faces_server.cpp`（跨站 Origin / `Sec-Fetch-Site: cross-site`
     → 403；同源与无头客户端 → 200）。
3. **清理明文回滚点**：加密迁移成功后，稳定运行期由运维人工删除
   `<数据库路径>.pre-encrypt`（有意保留的回滚点，见 SEC-03 §3.2）。
4. **密钥备份**：DPAPI 密钥丢失 = 识别库不可恢复；纳入运维备份策略。
5. **日志审计**：定期确认日志/错误体不含 token 与密钥明文（脱敏纪律）。

## 3. 验收说明（本任务边界）

- 本任务（SEC-16）**仅形成方案与本地文档**（`SECURITY.md`、
  `docs/security/enablement-plan.md`、`scripts/security-policy-audit.js`），
  **不触发任何远端功能**：不推送、不改 GitHub settings、不接入 CI。
- **远端验证需另行授权**：GitHub Actions 门禁接入、真实进程下的 401/403 行为、
  加密落盘抽查、`SECURITY.md` 在远端仓库可见性等，均须在获得授权后进行。
- 本地门禁演示：`node scripts/security-policy-audit.js --ci` → RED（缺文件，退出码 1）
  → 补 `SECURITY.md` 与本文档后 → GREEN（退出码 0）。
