<!-- DOCSYNC_DOC
type=reference
status=living
as_of=2026-09-15
-->

# 文档索引

本索引覆盖仓库中的全部文档。**文档规范见 [DOC_STANDARD.md](DOC_STANDARD.md)**（元数据、放置规则、单一事实源、引用纪律）。

- 表内路径相对于本文件所在目录（`docs/`）；仓库根目录文件以 `../` 前缀标注。
- **状态**列取自各文档头部的 `DOCSYNC_DOC` 元数据块：`living` 长期有效 · `snapshot` 时点快照 · `frozen` 已冻结 · `placeholder` 占位。
- 新增文档后请同步登记本表——门禁会校验索引完整性。

---

## 规范与索引

| 文件 | 状态 | 说明 |
|---|---|---|
| `index.md` | living | 本文件 |
| `DOC_STANDARD.md` | living | 文档规范：元数据、放置、单一事实源、引用纪律 |

## 参考（长期有效）

| 文件 | 状态 | 说明 |
|---|---|---|
| `RK3288_CONSTRAINTS.md` | living | RK3288 目标设备画像与工程硬约束清单 |
| `capability-matrix.md` | living | 终端能力矩阵（产物、能力、限制） |

## architecture/

| 文件 | 状态 | 说明 |
|---|---|---|
| `architecture/ARCHITECTURE.md` | living | 5 条跨子系统架构契约（含合规总表——注意当前 5/5 待修复） |
| `architecture/android-layer.md` | living | Android 层架构说明 |
| `architecture/cpp-engine.md` | living | C++ 核心引擎架构说明 |

## designs/

| 文件 | 状态 | 说明 |
|---|---|---|
| `designs/int8-quantization-design.md` | living | INT8 量化工具链架构设计 |
| `designs/personnel-enrollment-design.md` | living | 人员注册与权限系统顶层设计 |
| `designs/android-enrollment-design.md` | living | Android 端人员注册设计 |
| `designs/model-acquisition-import.md` | living | 模型获取与导入流程 |

## guides/

| 文件 | 状态 | 说明 |
|---|---|---|
| `guides/rk3288-device-acceptance.md` | living | RK3288 设备验收清单（人工执行） |
| `guides/windows-service/config.md` | living | Windows 服务配置格式、环境变量、加密与热重载 |
| `guides/windows-service/deploy_and_rollback.md` | living | Windows 服务部署与回滚指南 |
| `guides/windows-service/uat.md` | living | 用户验收测试场景清单 |
| `guides/windows-service/performance.md` | living | 性能验收口径与测量方法 |

## windows-web-spa/

| 文件 | 状态 | 说明 |
|---|---|---|
| `windows-web-spa/architecture.md` | living | 架构迁移说明（Win32 UI → 本地服务 + Web SPA） |
| `windows-web-spa/feature_parity.md` | living | Win32 旧 UI → Web SPA 功能对照清单 |
| `windows-web-spa/openapi.yaml` | — | REST API OpenAPI 规范（非 md，不在元数据要求内） |
| `windows-web-spa/config.schema.json` | — | Windows 配置 JSON Schema（非 md） |

## security/

| 文件 | 状态 | 说明 |
|---|---|---|
| `security/auth-model.md` | living | HTTP 认证模型（Bearer 令牌） |
| `security/face-db-encryption.md` | living | 人脸库加密（DPAPI + AES-256-GCM 版本化迁移） |
| `security/enablement-plan.md` | living | 安全能力启用计划与状态表（默认状态的唯一事实源） |
| `security/civetweb-ssl-note.md` | living | CivetWeb SSL 相关说明 |
| `security/supply-chain-ledger.md` | living | 外部二进制来源账本（版本/许可证/摘要） |

## bsp/

| 文件 | 状态 | 说明 |
|---|---|---|
| `bsp/BSP_EVIDENCE_STATUS.md` | living | BSP 证据登记与合规结论（**不是**厂商 Release Note，仓库尚未保存该文件） |
| `bsp/kernel-config/README.md` | living | 内核配置说明 |
| `bsp/kernel-config/kernel.config` | — | 设备运行态内核配置快照（非 md） |
| `bsp/defconfig/rk3288_defconfig` | — | 内核基准配置（非 md） |

## loop 与流程

| 文件 | 状态 | 说明 |
|---|---|---|
| `safety.md` | living | 安全与高风险路径清单（loop 人工门禁引用） |
| `patterns/daily-triage.md` | living | 每日分诊 loop 模式定义 |
| `patterns/loop-design-checklist.md` | living | loop 设计检查清单 |
| `../LOOP.md` | living | loop 配置（Daily Triage 1d / L1 report-only） |
| `../loop-constraints.md` | living | loop 绑定约束（每次运行前必读） |
| `../loop-budget.md` | living | loop 预算上限与超额处置 |
| `../loop-run-log.md` | living | loop 运行记录（按 run 追加） |
| `../STATE.md` | living | loop 运行时状态快照 |

## plans/（时点快照）

| 文件 | 状态 | 说明 |
|---|---|---|
| `plans/2026-07-03-triple-refactor-plan.md` | snapshot | 三层重构实施计划 |
| `plans/2026-07-04-governance-plan.md` | snapshot | 全项目治理实施计划 |
| `plans/2026-07-06-architecture-governance-plan.md` | snapshot | 架构治理实施计划 |
| `plans/2026-07-06-docs-compliance-plan.md` | snapshot | 文档合规推进计划 |
| `plans/2026-07-18-win-ci-optimization-audit.md` | snapshot | Windows CI 优化审计 |
| `plans/2026-07-18-win-ci-speedup.md` | snapshot | Windows CI 提速实施计划 |
| `plans/2026-07-21-agent-guide-audit.md` | snapshot | 助手指南（AGENTS.md/CLAUDE.md）审计与镜像治理 |
| `plans/int8-quantization-plan.md` | snapshot | INT8 量化实现计划（任务级） |

## specs/（时点快照）

| 文件 | 状态 | 说明 |
|---|---|---|
| `specs/2026-07-03-triple-refactor-design.md` | snapshot | 三层重构设计（Engine/FramePipeline/HttpFacesServer 值流管线） |
| `specs/2026-07-04-governance-plan-design.md` | snapshot | 全项目治理计划设计 |
| `specs/2026-07-06-architecture-governance-design.md` | snapshot | 架构治理设计（5 条契约的**目标**定义） |

## audits/

| 文件 | 状态 | 说明 |
|---|---|---|
| `audits/2026-09-15-documentation-audit.md` | snapshot | 全项目文档审计报告（门禁基准失效根因、真实错误清单、遗留问题） |

### audits/archive/（已冻结）

历史审计快照，保留原始内容不再更新。2026-09-15 由仓库根目录归档至此。

| 文件 | 状态 | 说明 |
|---|---|---|
| `audits/archive/AUDIT_REPORT.md` | frozen | 全项目审计报告（2026-07-04，综合评分 2.9/5，38 项发现） |
| `audits/archive/AUDIT_RESULT.md` | frozen | 针对《重构洞察.txt》的逐条验证结果 |
| `audits/archive/GITHUB_SCAN_AUDIT.md` | frozen | GitHub 安全与质量扫描审计（CodeQL + Dependabot，2026-07-05） |
| `audits/archive/GOVERNANCE_VERIFICATION.md` | frozen | 架构治理落地验证报告（2026-07-05） |
| `audits/archive/GOVERNANCE_EXECUTION_AUDIT.md` | frozen | 治理执行审计（基准 master@4b4e0e2，2026-07-19） |
| `audits/archive/REMOTE_BRANCH_AUDIT.md` | frozen | 远程分支审计与清理记录（2026-07-06） |

## research/（时点快照）

拆分自 [DEVELOP.md](../DEVELOP.md) 附录（2026-07-05），DEVELOP.md 中仅保留指向这些文件的锚点。

| 文件 | 状态 | 说明 |
|---|---|---|
| `research/appendix-b.md` | snapshot | 加速方案可行性分析与实现状态 |
| `research/appendix-c.md` | snapshot | Android 摄像头调用机制研究 |
| `research/appendix-d.md` | snapshot | 人脸识别技术实现方案研究 |
| `research/appendix-e.md` | snapshot | 性能优化与故障排障研究 |
| `research/appendix-f.md` | snapshot | 证据日志分析报告 |
| `research/appendix-g.md` | snapshot | 稳定性治理审计修复计划 |
| `research/appendix-h.md` | placeholder | 稳定性治理分阶段执行报告（内容未填充） |
| `research/opensource-benchmark-2026-09-16.md` | snapshot | 开源对标调研（核实版）：重复开发 / 独有 / 可借鉴三分类与行动建议 |
| `research/opensource-integration-feasibility-2026-09-16.md` | snapshot | 开源代码可集成性评估：四档可行性分级、语言适配矩阵、不建议清单、32 位移植代价 |

## 仓库根目录

| 文件 | 状态 | 说明 |
|---|---|---|
| `../README.md` | living | 项目简介与快速开始 |
| `../DEVELOP.md` | living | 系统架构、构建、测试、工作流规范 |
| `../CREDITS.md` | living | 完整依赖清单与模型台账 |
| `../SECURITY.md` | living | 安全策略与漏洞报告渠道 |
| `../CHANGELOG.md` | living | 变更记录（Keep a Changelog 格式） |
| `../AGENTS.md` | living | AI 助手指南（事实源，含机器可读事实块） |
| `../CLAUDE.md` | living | AI 助手指南镜像（共享区间与 AGENTS.md 校验一致） |
| `../.claude/CLAUDE.md` | living | Claude Code 子目录指南（graphify 触发 + 记忆系统指针） |

## 子系统说明（各自目录内）

| 文件 | 状态 | 说明 |
|---|---|---|
| `../web/README.md` | living | Web SPA 构建、测试与产物路径 |
| `../src/win/third_party/civetweb/LEDGER.md` | living | CivetWeb 供应链台账 |
| `../deps/qualcomm_snpe/README.md` | — | 第三方依赖说明（**豁免**元数据要求） |
| `../deps/rk_mpp/README.md` | — | 第三方依赖说明（**豁免**元数据要求） |
| `../src/win/third_party/civetweb/LICENSE.md` | — | 第三方许可证（**豁免**元数据要求） |

## 工具配置（豁免元数据要求）

`.claude/agents/` 下的 5 份 agent 定义（`android.md`、`cpp.md`、`web.md`、`win.md`、`loop-verifier.md`）是工具配置而非项目文档，不参与文档规范。

> **未入库的工作台账**：仓库根目录另有 `FULL_PROJECT_AUDIT_2026-07-22.md` 与
> `CLAUDECODE_DEEPSEEKV4FLASH_GOVERNANCE_TASKS_2026-07-22.md` 两份大型工作文档，
> 其文件头自述不进入版本控制，因此未被 git 跟踪、不受文档门禁约束，也不登记在本索引中。
