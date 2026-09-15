# 文档索引

**更新日期**: 2026-09-15

> 本索引覆盖 `docs/` 与仓库根目录下的全部文档。新增文档或模块结构变更后请同步维护本表，
> 避免与 `docs/` 实际内容脱节（`docs-sync-audit` 门禁会校验本文件的路径引用是否存在）。
> 表内路径相对于本文件所在目录（`docs/`）；仓库根目录文件以 `../` 前缀标注。

## docs/ 根级
| 文件 | 说明 |
|------|------|
| `RK3288_CONSTRAINTS.md` | RK3288 目标设备画像与工程硬约束清单 |
| `capability-matrix.md` | 终端能力矩阵（Android/Windows/Web/旧 Win32 UI + 5 个 CLI 的产物、能力、限制） |
| `safety.md` | 安全与高风险路径清单（loop 人工门禁引用） |

## windows-web-spa/
| 文件 | 说明 |
|------|------|
| `architecture.md` | 架构迁移说明（Win32 UI → 本地服务 + Web SPA） |
| `config.md` | 配置格式、环境变量、加密与热重载 |
| `config.schema.json` | JSON Schema 校验文件 |
| `deploy_and_rollback.md` | 部署与回滚指南 |
| `feature_parity.md` | Win32 旧 UI → Web SPA 功能对照清单 |
| `openapi.yaml` | REST API OpenAPI 规范 |
| `performance.md` | 性能验收口径与测量方法 |
| `uat.md` | 用户验收测试场景清单 |

## architecture/
| 文件 | 说明 |
|------|------|
| `ARCHITECTURE.md` | 5 条跨子系统架构契约（RAII/纯函数管线/原子提交/错误通道/接口隔离） |
| `android-layer.md` | Android 层架构说明 |
| `cpp-engine.md` | C++ 核心引擎架构说明 |

## security/
| 文件 | 说明 |
|------|------|
| `auth-model.md` | HTTP 认证模型（Bearer 令牌） |
| `civetweb-ssl-note.md` | CivetWeb SSL 相关说明 |
| `enablement-plan.md` | 安全能力启用计划与状态表 |
| `face-db-encryption.md` | 人脸库加密（DPAPI + AES-256-GCM 版本化迁移） |
| `supply-chain-ledger.md` | 外部二进制来源账本（版本/许可证/摘要） |

## designs/
| 文件 | 说明 |
|------|------|
| `int8-quantization-design.md` | INT8 量化工具链架构设计 |
| `int8-quantization-plan.md` | INT8 量化实现计划（任务级） |
| `personnel-enrollment-design.md` | 人员注册与权限系统顶层设计 |
| `android-enrollment-design.md` | Android 端人员注册设计（目标 vs 已落地，任务 17/18 边界） |
| `model-acquisition-import.md` | 模型获取与导入流程（APK 不内置模型、ZIP 校验链、`missing_model` 降级） |
| `rk3288-device-acceptance.md` | RK3288 Android 设备验收清单（人工执行） |

## patterns/
| 文件 | 说明 |
|------|------|
| `daily-triage.md` | 每日分诊 loop 模式定义 |
| `loop-design-checklist.md` | loop 设计检查清单 |

## bsp/
| 文件 | 说明 |
|------|------|
| `BSP_RELEASE_NOTES.md` | BSP 发布说明（占位模板） |
| `kernel-config/README.md` | 内核配置说明 |
| `kernel-config/kernel.config` | 设备运行态内核配置快照 |
| `defconfig/rk3288_defconfig` | 内核基准配置 |

## superpowers/specs/
| 文件 | 说明 |
|------|------|
| `2026-07-03-triple-refactor-design.md` | 三层重构设计文档（Engine/FramePipeline/HttpFacesServer 值流管线） |
| `2026-07-04-governance-plan-design.md` | 全项目治理计划设计 |
| `2026-07-06-architecture-governance-design.md` | 架构治理设计文档 |

## superpowers/plans/
| 文件 | 说明 |
|------|------|
| `2026-07-03-triple-refactor-plan.md` | 三层重构实施计划 |
| `2026-07-04-governance-plan.md` | 全项目治理实施计划 |
| `2026-07-06-architecture-governance-plan.md` | 架构治理实施计划 |
| `2026-07-06-docs-compliance-plan.md` | 文档合规推进计划 |
| `2026-07-18-win-ci-optimization-audit.md` | Windows CI 优化审计 |
| `2026-07-18-win-ci-speedup.md` | Windows CI 提速实施计划 |
| `2026-07-21-agent-guide-audit.md` | 助手指南（AGENTS.md/CLAUDE.md）审计与镜像治理 |

## research/
拆分自 [DEVELOP.md](../DEVELOP.md) 附录（2026-07-05），DEVELOP.md 中仅保留指向这些文件的锚点。

| 文件 | 说明 |
|------|------|
| `appendix-b.md` | 加速方案可行性分析与实现状态 |
| `appendix-c.md` | Android 摄像头调用机制研究 |
| `appendix-d.md` | 人脸识别技术实现方案研究 |
| `appendix-e.md` | 性能优化与故障排障研究 |
| `appendix-f.md` | 证据日志分析报告 |
| `appendix-g.md` | 稳定性治理审计修复计划 |
| `appendix-h.md` | 稳定性治理分阶段执行报告（占位文档，内容未填充） |

## audits/
| 文件 | 说明 |
|------|------|
| `2026-09-15-documentation-audit.md` | 全项目文档审计报告（门禁基准失效根因、真实错误清单、门禁改造与遗留问题） |

### audits/archive/
历史审计快照，保留原始内容不再更新。原位于仓库根目录，2026-09-15 归档至此
（归档说明见 `2026-09-15-documentation-audit.md` 第 5 节）。

| 文件 | 说明 |
|------|------|
| `archive/AUDIT_REPORT.md` | 全项目审计报告（审计日期 2026-07-04，综合评分 2.9/5，38 项发现） |
| `archive/AUDIT_RESULT.md` | 针对《重构洞察.txt》的逐条验证结果 |
| `archive/GITHUB_SCAN_AUDIT.md` | GitHub 安全与质量扫描审计（CodeQL + Dependabot，2026-07-05） |
| `archive/GOVERNANCE_VERIFICATION.md` | 架构治理落地验证报告（2026-07-05） |
| `archive/GOVERNANCE_EXECUTION_AUDIT.md` | 治理执行审计（基准 master@4b4e0e2，2026-07-19） |
| `archive/REMOTE_BRANCH_AUDIT.md` | 远程分支审计与清理记录（2026-07-06） |

## 仓库根目录
| 文件 | 说明 |
|------|------|
| `../README.md` | 项目简介与快速开始 |
| `../DEVELOP.md` | 系统架构、构建、测试、工作流规范 |
| `../CREDITS.md` | 完整依赖清单与模型台账 |
| `../SECURITY.md` | 安全策略与漏洞报告渠道 |
| `../CHANGELOG.md` | 变更记录（Keep a Changelog 格式） |
| `../AGENTS.md` | AI 助手指南（事实源，机器可读事实块） |
| `../CLAUDE.md` | AI 助手指南镜像（与 AGENTS.md 共享区间校验一致） |

## 根目录 loop 脚手架
由 `loop-init` 生成，供 `/loop` 调度框架使用。

| 文件 | 说明 |
|------|------|
| `../LOOP.md` | loop 配置（Daily Triage 1d / L1 report-only） |
| `../loop-constraints.md` | loop 绑定约束（每次运行前必读） |
| `../loop-budget.md` | loop 预算上限与超额处置 |
| `../loop-run-log.md` | loop 运行记录（按 run 追加） |
| `../STATE.md` | loop 运行时状态快照 |

> 注：根目录另有 `FULL_PROJECT_AUDIT_2026-07-22.md` 与
> `CLAUDECODE_DEEPSEEKV4FLASH_GOVERNANCE_TASKS_2026-07-22.md` 两份大型工作文档，
> 其文件头自述不进入版本控制，因此**未被 git 跟踪**，也不受 `docs-sync-audit` 门禁约束。
