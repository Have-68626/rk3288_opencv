<!-- DOCSYNC_DOC
type=security
status=living
as_of=2026-08-13
-->

# HTTP 认证授权模型（S-SEC-B09-001）

> 规格状态：**已锁定**。本文档是 Windows 本地服务 REST API 认证授权的事实源；
> 正文跟随实现更新（元数据 `status=living`），但规格条款未经批准不得变更。
> 任何机制/边界变更必须先更新本文档并同步实现与测试。
> 治理回退：仅允许撤回本项 auth patch；认证模型冻结后不得在未重新冻结前继续扩展。

## 1. 背景与风险

Windows 本地服务（`win_local_service`，HttpFacesServer，Winsock）提供的 REST API
当前**全匿名**可达：

- `EndpointRegistry::dispatch`（`src/win/src/EndpointRegistry.cpp:9-24`）无鉴权；
- 注册点 `registerAllHttpApi`（`src/win/src/JsonEndpointHandlers.cpp:405-436`）
  中的写操作（PUT settings、POST actions）与流式端点（SSE/MJPEG，经
  `HttpFacesServer.cpp:605-611` StreamSessionRunner 旁路）均匿名可达；
- 服务仅 bind `127.0.0.1`，但本机任意进程/用户仍可调用（包括浏览器访问已打开的
  前端页面后调用管理 API、恶意本机进程扫描、以及 DNS-rebinding 类绕过）。

写操作可：篡改配置（settings）、清空人脸库、注册/删除人脸、重载模型、轮换加密密钥、
打开隐私设置；流可暴露相机画面与人脸识别结果。均属需要授权的管理动作。

## 2. 认证机制（已冻结）

- **方案**：HTTP **Bearer Token**（`Authorization: Bearer <token>` 请求头）。
  遵循 RFC 6750 的 Bearer 用法。
- **Token 来源**：环境变量 **`RK_WCFR_HTTP_AUTH_TOKEN`**，在
  `HttpFacesServer::start()` 时读取一次并缓存。
  - **配置字段 `http.authToken` 本项不实现**，理由：`docs/windows-web-spa/config.schema.json`
    的 `http` 段为 `additionalProperties:false` 且仅含 `enable`/`port`，新增字段须同步
    schema + 序列化 + 脱敏回写 + 相关测试，超出本项允许路径（config.schema 不在
    `docs/security/` 安全规格路径内）。**此扩展登记为待办，交由 CONFIG 任务承接**。
    届时 token 读取优先级应保持"配置字段 > 环境变量"或显式覆盖，并同步本文档。
- **Token 保护要求**：
  - 日志、错误体、脱敏输出**不得**记录 token 明文（见 §6 脱敏纪律）；
  - 比较使用**恒定时间比较**（见 §4），防时序侧信道。

## 3. 授权边界（最小权限，已冻结）

### 3.1 匿名白名单（无需认证）

| 路径 | 原因 |
|---|---|
| 静态文件（非 `/api` 前缀，如 `/`、`/assets/*`、`/index.html`） | 前端资源，无敏感动作；写操作被 `handleStaticOrFallback` 以 405 拒绝 |
| `GET /api/v1/health` | 健康检查：回显服务名、端口，以及**启动期配置诊断**三字段（`configStatus` / `configWarning` / `configQuarantinedTo`，见下方「健康端点的启动期配置诊断」），供进程监督/负载探测 |
| `GET /api/v1/acceleration` | 只读状态：当前加速配置与活动后端摘要，不含密钥/凭据 |

> 判定函数：非 `/api` 前缀恒匿名；`/api` 下仅上述两个 GET 端点匿名。

#### 健康端点的启动期配置诊断（FACE-CONFIG-001，2026-09-23 追加；**本小节为规格条款变更**）

原文为「仅回显服务名与端口」。现追加三字段，动机见 `docs/guides/windows-service/config.md`：
配置装载失败需要一个**不依赖被拒那份配置**的可见通道。

- `configStatus`（**恒出现**）：`ok` / `degraded` / `rejected` / `unknown`。`rejected` = 既有 `config.json`
  存在但无法装载（不可读 / JSON 解析失败 / schema 校验被拒），服务以默认值启动；`degraded` = 装载完成但
  本次启动存在告警（默认配置写盘失败 / 序列化未通过 schema 等，即 `initWarning` 非空）；`ok` = 正常装载
  且无告警；`unknown` = 诊断通道缺失（`ctx.settings` 为空，无法判定配置状态，仅内部测试路径可达）。
  判据优先级：`unknown` > `rejected` > `degraded` > `ok`。
- `configWarning`（**仅本次启动有告警时出现**）：服务端告警原文。
- `configQuarantinedTo`（**仅发生隔离时出现**）：被改名另存的坏配置**文件名**（非全路径）。

边界（据此仍维持 §3.1 的最小权限口径）：

- 端点仍**仅监听 127.0.0.1**，仍**只读**、无写能力、不含密钥/凭据；
- 新增字段**不回读被拒的那份配置**——只读 `initialize` 写入的三个内存标志，故配置不可信时它依然有效
  （这正是「独立可见通道」的含义）；
- ⚠️ `configWarning` 是服务端原文，**可能含文件系统路径**，故本端点内容应按**本机可见**对待；
- 已按本文件 §1 的冻结流程同步实现与测试（`WinJsonConfig` 的三个诊断访问器 +
  `JsonEndpointHandlers::handleGetHealth`；用例 `HttpFacesAuth.HealthExposesConfigStatusOk`、
  `HttpFacesAuth.HealthExposesDegradedConfigStatus`、`HttpFacesAuth.HealthExposesRejectedConfigStatus`
  与 `HttpFacesAuth.AnonymousWhitelistRemainsPublic`——后者以 `store=nullptr` 断言
  `configStatus:"unknown"`，补齐四态证据）。
- ⚠️ `configStatus` 取值集合的历次扩张：`ok` / `rejected` → 增 `unknown`（诊断通道缺失）→ 增 `degraded`
  （装载完成但本次启动有告警）。其中 `unknown` 为**事后同步**（`handleGetHealth` 先行改为「settings 为空时
  置 `unknown`、不再回落 `ok`」，本小节随后回填）；`degraded` 为**事前改文档**——本轮先在 §3.1 定稿四态
  取值与判据，再据此改 `JsonEndpointHandlers::handleGetHealth` 并补用例
  `HttpFacesAuth.HealthExposesDegradedConfigStatus`。

### 3.2 需认证（启用认证后全部需有效 token）

- **所有写操作**：`PUT /api/v1/settings`、`PUT /api/v1/camera/flip`、
  `POST /api/v1/models/reload`、`POST /api/v1/actions/*`（enroll / db/clear /
  crypto/rotate / privacy/open）。
- **管理类读操作**：`GET /api/v1/models`、`GET /api/v1/settings`（含路径/配置，
  虽脱敏仍属内部信息）、`GET /api/v1/cameras`、`GET /api/faces`、
  `GET /api/v1/faces`（人脸库快照，隐私敏感）、`GET /api/v1/preview.jpg`（相机画面）。
- **流式端点**（SSE / MJPEG）：`/api/faces/stream`、`/api/v1/faces/stream`、
  `/api/v1/preview.mjpeg` —— 流暴露相机画面与人脸结果，必须认证。
- **OpenAPI 文档**：`/api/v1/openapi`、`/openapi.json` —— 管理面文档，一并纳入认证
  （不属白名单；若前端需匿名拉取，属后续授权细分，须先更新本文档）。

> 授权规则一句话：**启用认证后，除 §3.1 白名单外的一切 `/api` 请求（含流）必须携带
> 有效 Bearer token**。读写统一收口，最小权限且无遗漏路径。

### 3.3 默认行为（未配置 token）

`RK_WCFR_HTTP_AUTH_TOKEN` 为空/未设置 → **认证未启用（兼容模式）**，行为与修复前
一致（匿名可达），保证既有部署与 Web SPA 不回归。

- 此模式为**记录在案的残余风险**：本机仍可匿名调用管理 API。
- 生产部署应设置 token 以启用保护；本文档即"启用说明"。
- 测试按"未配置=放行、配置后=强制"双态覆盖（见 §5）。

### 3.4 未认证/认证失败响应（已冻结）

- 状态码：**401**。
- 响应体与既有错误格式一致：`{"ok":false,"error":{"code":"unauthorized",
  "message":"需要有效的认证令牌"}}`（含既有安全响应头 X-Content-Type-Options 等）。
- **不发送 `WWW-Authenticate` 头**：避免暴露 token 机制/触发无谓认证握手循环；
  401 + JSON 错误体已足以指示客户端补 token。若将来需要标准协商，先更新本文档。

### 3.5 跨站请求伪造（CSRF）防护（S-SEC-B09-004，已冻结）

> 本项（SEC-07）在认证收口之上叠加 CSRF 校验，属**认证之外的第二道纵深**。
> 机制与边界冻结于此；变更须先更新本文档并同步实现与测试。

**背景**：服务仅 bind `127.0.0.1`，本机浏览器访问已打开的管理页面后，可能被恶意
网页诱导发出**跨站请求**（表单 POST / 脚本 img / fetch）到 `127.0.0.1:8080`。
Bearer token 只阻止"无凭据"请求，无法阻止"浏览器自动携带凭据+发起的跨站写"——
故对写方法须独立校验请求来源。

**判定函数**：`HttpFacesServer::csrfCheckPasses`（`src/win/src/HttpFacesServer.cpp`），
仅对**写方法（POST/PUT/DELETE）**生效；读方法与 GET 恒放行（无副作用）。

决策树（按序判定，命中即返回）：

| # | 条件 | 结果 |
|---|---|---|
| 1 | 非写方法（GET/HEAD 等） | **放行** |
| 2 | `Sec-Fetch-Site: cross-site` | **拒绝（403）** |
| 3 | `Sec-Fetch-Site` 存在但非 cross-site（same-origin/same-site/none） | **放行** |
| 4 | `Origin` 缺失 → **放行**；空值 → **拒绝（403）**（curl/API 客户端/旧浏览器写：缺失放行；空 Origin 同 `"null"` 属无源上下文，按 §5 原则拒绝） |
| 5 | `Origin` 为字面量 `"null"` | **拒绝（403）**（沙箱 iframe / data: 等无源上下文） |
| 6 | `Origin` 的 host 为 `127.0.0.1`/`localhost`/`::1`（http/https，任意端口） | **放行** |
| 7 | 其余一切 `Origin` | **拒绝（403）** |

要点与残余风险：

- `Sec-Fetch-Site` 由浏览器设置、**不可被 JS 伪造**，是比 Origin 更强的现代信号；
  缺失时回退 Origin（旧浏览器兼容）。
- 回环 host **精确匹配**（非子串）：`localhost.evil.com`、`127.0.0.1.evil.com`
  均不命中白名单 → 拒绝，防 DNS-rebinding 类绕过。
- 忽略端口：本机回环任意端口均同机，且兼容 Vite 开发代理（浏览器源为
  `localhost:<devPort>`）。
- 两头部均缺失 → 放行：保证 curl / API 客户端 / 旧浏览器写不被破坏（与 §3.3
  兼容模式一致的残余风险：无法区分"旧浏览器跨站写"与"本机非浏览器客户端"）。
- 拒绝响应：**403**，错误码 `cross_site_forbidden`，不发送 CORS 头、不回显请求中的
  Origin/token（脱敏纪律 §6）。
- 校验顺序：位于认证收口**之后**（§4 注入点同一处），先 401 后 403；未配置 token
  的兼容模式下 CSRF 校验**仍然生效**（认证与 CSRF 正交，纵深不因兼容模式而关闭）。

## 4. 实现要求（最小实现）

- **注入点**：`HttpFacesServer::handleClient`，位于请求解析之后、
  **StreamSessionRunner 旁路与 `handleRequest` 分发之前** —— 单一收口同时覆盖
  `EndpointRegistry` 路由与流旁路，避免"注册表已锁但流漏放行"。
- 校验顺序：认证未启用 → 放行；匿名白名单路径 → 放行；其余提取
  `Authorization: Bearer <token>`（scheme 大小写不敏感，token 按原文）→
  **恒定时间比较**：
  ```cpp
  // 长度不同直接不等（长度本身属可接受的信息泄漏）；同长时逐字节 XOR 归并，
  // 不提前退出，避免基于首个差异字节的时序侧信道。
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
      diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  return diff == 0;
  ```
- 相等 → 放行；不等/缺失 → 401（§3.4）。
- 认证失败不执行任何 handler（写操作零副作用）。

## 5. 测试契约（对应测试文件：`tests/win/test_http_faces_server.cpp`）

| 用例 | 前置 | 请求 | 期望 |
|---|---|---|---|
| 未认证写可达（exploit/兼容） | 无 token | `PUT /api/v1/settings` 无 Authorization | 200（修复前后恒成立，记录残余风险） |
| 未认证写拒绝（RED→GREEN） | token=test 值 | `PUT /api/v1/settings` 无 Authorization | **401** |
| 未认证动作拒绝 | token=test 值 | `POST /api/v1/actions/enroll` 无 Authorization | **401** |
| 错误 token 拒绝 | token=test 值 | `GET /api/v1/models` Bearer wrong | **401** |
| 有效 token 通过 | token=test 值 | `GET /api/v1/models` Bearer test 值 | **200** |
| 白名单仍匿名 | token=test 值 | `GET /api/v1/health`、`/api/v1/acceleration` 无 Authorization | **200** |
| 流需认证 | token=test 值 | `GET /api/v1/faces/stream` 无 Authorization | **401** |
| 认证逻辑单测 | token=test 值 | `requestAuthorized()` 矩阵（有效/错误/缺失/白名单/未启用） | 见用例断言 |
| 跨站 Origin 写拒绝（RED→GREEN） | token=test 值 | `PUT /api/v1/settings` Bearer 有效 + `Origin: https://evil.example` | **403** |
| 跨站表单 POST 拒绝 | token=test 值 | `POST /api/v1/actions/enroll` Bearer 有效 + `Origin: https://evil.example`（表单类型） | **403** |
| Sec-Fetch-Site 跨站拒绝 | token=test 值 | `PUT /api/v1/settings` Bearer 有效 + `Sec-Fetch-Site: cross-site` | **403** |
| 同源写放行 | token=test 值 | `PUT /api/v1/settings` Bearer 有效 + `Origin: http://127.0.0.1:*` 或 `http://localhost:*` | **200** |
| 同源 + Sec-Fetch-Site 放行 | token=test 值 | `PUT /api/v1/settings` Bearer 有效 + `Origin: http://127.0.0.1:8080` + `Sec-Fetch-Site: same-origin` | **200** |
| 无头客户端写放行 | token=test 值 | `PUT /api/v1/settings` Bearer 有效，无 Origin/Sec-Fetch-Site | **200** |

## 6. 脱敏纪律

- token 仅存在于环境变量与进程内存；不写入日志、错误体、脱敏配置输出。
- 401 响应体不回显请求头中的 token。
- 测试使用非敏感占位 token（如 `test-secret-token`），且测试进程内设置/清除环境变量
  不污染其他用例。

## 7. 验收与回退

- 验收（认证）：未认证拒绝（401）、最小权限通过（白名单匿名 + 有效 token 通过）；既有测试
  适配后不删改语义（注册表层测试因认证在 handleClient 收口而天然不受影响）。
- 验收（CSRF，SEC-07）：跨站 Origin / `Sec-Fetch-Site: cross-site` 的**已认证写请求**拒绝
  （403）；合法同源写、curl/API 客户端（无 Origin/Sec-Fetch-Site）写通过（200）；认证与
  CSRF 判定顺序为 401 先于 403，兼容模式（无 token）下 CSRF 仍生效。
- 回退：仅撤回本项认证 patch 即恢复原行为；**认证模型冻结后不得在不重冻结的前提下
  继续扩展**（如加角色/加配置字段）。CSRF 为独立补丁（SEC-07），可单独回退而不影响 §2-§4。

## 8. 流会话配额（S-SEC-B09-005，已冻结）

> 本项在认证/CSRF 收口之上，为流式端点（SSE/MJPEG）叠加**每会话资源配额**，
> 防止单个/多个流会话消耗无界 CPU / 字节 / 时长。机制与边界冻结于此。

**背景**：全局 `ConnectionQuota=64`（HttpFacesServer，`src/win/src/HttpFacesServer.cpp`）
只限制**并发连接数**，不限制单个流会话的**持续消耗**：
- MJPEG 可被恶意客户端迫使按源帧率（30+ fps）连续 JPEG 编码（CPU 无界）；
- SSE/MJPEG 会话可被**无限期持有**（线程占用，且与全局配额争夺全部 64 个连接槽位）。

**预算依据（产品）**：
- **MJPEG FPS 预算 = 20fps**（帧间最小间隔 50ms）。依据 `docs/RK3288_CONSTRAINTS.md`
  §1.3/§1.6：相机输入 720p@15-30fps；预览通道人眼 ≥15fps 已平滑，20fps 把每流
  JPEG 编码 CPU 封顶（720p q80 ≈ 20ms/帧 → 20fps ≈ 单核 40%），恶意客户端无法
  迫使服务端按源帧率编码。
- **单会话字节配额 = 8GiB（回退硬顶）**。依据：20fps × 720p（q80 约 100-250KB/帧）
  ≈ 2-5MB/s，8GiB 对应连续预览 30-60 分钟，远超正常预览会话；仅拦截失控/被劫持
  会话无限产出。
- **会话时长 = 30 分钟（超时终止）**。独立于全局 ConnectionQuota 的流级时长限制：
  SSE（EventSource）断线自动重连，30 分钟刷新无缝；MJPEG `<img>` 无自动重连，
  正常连续预览短于该值，超过后需用户刷新（预览页已有"刷新预览"兜底）。
- 三者共同约束：单会话总产出 ≤ 字节配额、每流编码速率 ≤ FPS 预算、会话存活 ≤
  时长配额 —— 64 连接攻击下**总工作有界**（连接数 × 每流预算）。

**实现位置**：`src/win/include/rk_win/StreamSessionRunner.h`（`StreamLimits` 结构，
生产默认值带 WHY 注释）与 `src/win/src/StreamSessionRunner.cpp`（`writeRaw` 字节记账、
`sessionExpired` 时长判定、`runSse`/`runMjpeg` 循环内超时检查、`runMjpeg` FPS 节流）。
HttpFacesServer 构造 runner 时使用默认预算，无需改接线（配额完全内聚于 StreamSessionRunner）。

**测试契约**（`tests/win/test_http_faces_server.cpp` 的 `HttpFacesStreamQuota.*`，
直接驱动 StreamSessionRunner + 本机回环 socket 对，注入极小配额保证确定性）：

| 用例 | 注入预算 | 期望（判别） |
|---|---|---|
| MjpegFpsThrottle | fps=10（100ms 间隔） | 1.5s 窗口内帧数 ≤ 20（超预算写入被节流）且 ≥ 3（正常流可用） |
| MjpegByteQuotaTerminatesSession | 4KiB 字节配额 | 会话自行终止（不再依赖 running=false），写出 ≤ 配额+余量，仍收到 HTTP 200 头 |
| SseDurationTerminatesSession | 250ms 时长 | SSE 会话 3s 内自行断开，断开前已收到 200 / text/event-stream 头 |

**回退**：仅撤回本项 StreamSessionRunner 配额 patch 即恢复原行为（流无节流/无超时）。
配额默认值变更须先更新本文档（同 §2 口径：先更新规格再同步实现与测试）。
