# 安全策略（Security Policy）

> 本文档是仓库级安全治理的事实源（治理任务 S-SEC-B10-007 / SEC-16）。
> 范围覆盖四个子系统：Android App、Windows 服务、Web SPA、C++ 核心引擎。
> 安全机制规格（冻结）见 `docs/security/*`；默认开/关与启用建议见
> `docs/security/enablement-plan.md`（最小启用方案）。

## 报告安全漏洞

- **报告渠道**：通过 GitHub 仓库提交 issue 报告漏洞；如需私密披露，请在 issue 中
  注明"安全披露"，维护者将私信联系。仓库当前无自动披露邮箱。
- **报告内容**：受影响子系统与版本、可复现步骤（含最小样例）、预期行为 vs 实际行为、
  可能的缓解措施、是否已公开。
- **处理承诺**：
  - 收到后 1 个工作日内确认；
  - 3 个工作日内完成分级（严重 / 高 / 中 / 低）；
  - 严重 / 高危：确认后优先排期修复。
- **协调披露**：修复发布前不公开漏洞细节，避免在途攻击利用。
- **范围外**：冻结规格（`docs/security/auth-model.md`、`face-db-encryption.md`）中
  已书面记录的**残余风险**（如认证兼容模式匿名可达、加密迁移的明文回滚点）属已知
  有意保留项，不在漏洞受理范围，但生产部署应按最小启用方案缓解。

## 安全边界

- Windows 本地服务（`HttpFacesServer`，Winsock）仅 bind `127.0.0.1`，面向本机浏览器
  与进程；远端默认不可达（除非显式端口转发）。**本机任意进程/用户仍可访问**，属
  威胁模型内的本机攻击面。
- 人脸特征库与配置的密钥受 **DPAPI（当前 Windows 用户）** 保护；密钥丢失/损坏 =
  数据不可恢复，生产需备份策略。
- 人脸库加密迁移时保留 `.pre-encrypt` 明文回滚点（有意保留，见 SEC-03）；稳定运行
  期后应由运维人工删除。
- 三个子系统共享人脸识别数据流：Windows 服务持有识别库与相机流，Web SPA 仅通过
  本机 REST API 消费，Android App 为独立采集/识别端。

## 安全功能清单与默认状态

| 安全功能 | 规格来源 | 默认状态 | 启用方式 |
|---|---|---|---|
| REST API 认证（Bearer token） | `docs/security/auth-model.md`（SEC-02） | **关闭（兼容模式，匿名放行）** | 设置环境变量 `RK_WCFR_HTTP_AUTH_TOKEN` 后重启服务 |
| CSRF 防护（写方法 POST/PUT/DELETE） | `auth-model.md` §3.5（SEC-07） | **开启（无条件，兼容模式下仍生效）** | 无需配置 |
| 人脸特征库加密（AES-256-GCM + DPAPI） | `docs/security/face-db-encryption.md`（SEC-03） | **开启（无条件，save 一律加密、load 自动迁移）** | 无需配置 |
| 日志敏感字段脱敏 | S-SEC-B07-001（NativeLog） | 开启 | 无需配置 |
| 日志 URI userinfo 脱敏 | S-SEC-B07-002（sanitizeUriForLog） | 开启 | 无需配置 |
| JNI/整数溢出防护（32 位 checked arithmetic） | S-SEC-B09-003 | 开启 | 无需配置 |
| CivetWeb TLS 证书校验（`SSL_VERIFY_PEER`） | `docs/security/civetweb-ssl-note.md` | 不适用（CivetWeb 未编入构建） | 重新接入前强制改为 `SSL_VERIFY_PEER` |

> 默认关闭的功能必须按最小启用方案显式启用；默认开启的功能不得在不更新对应冻结规格
> 的前提下关闭（认证/加密/CSRF 规格已冻结，见各文档"状态：已冻结"）。

## 文档映射

| 文档 | 内容 |
|---|---|
| `docs/security/auth-model.md` | REST API 认证授权模型（冻结）+ CSRF 决策树（冻结） |
| `docs/security/face-db-encryption.md` | FaceDatabase 加密规格、密钥边界、迁移与回滚（冻结） |
| `docs/security/enablement-plan.md` | 最小启用方案：默认开/关清单 + 启用建议 + 验收说明 |
| `docs/security/civetweb-ssl-note.md` | CivetWeb SSL 校验归档说明（当前不适用） |

## 变更纪律

- 冻结规格的机制变更必须先更新本文档与对应规格，再实施代码（见各规格"治理回退"条款）。
- 本文档与 `docs/security/enablement-plan.md` 由 `scripts/security-policy-audit.js`
  门禁守护：任一文件缺失或章节不全即门禁 RED（退出码 1）。
- 远端功能验证（GitHub 门禁接入、实际 401/403/加密落盘检查）需另行授权后进行。
