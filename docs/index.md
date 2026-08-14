# 文档索引

> 索引快照说明：本索引反映 2026-07 三层重构（Engine/FramePipeline/HttpFacesServer）后的文档基线，
> 覆盖 windows-web-spa、架构、治理设计/计划、人员注册设计等模块；最近更新 2026-08-10（Q4 治理轮）。
> 新增文档或模块结构变更后请同步维护本表，避免与 docs/ 实际内容脱节。

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

## designs/
| 文件 | 说明 |
|------|------|
| `int8-quantization-design.md` | INT8 量化工具链架构设计 |
| `int8-quantization-plan.md` | INT8 量化实现计划（任务级） |
| `personnel-enrollment-design.md` | 人员注册与权限系统顶层设计 |

## bsp/
| 文件 | 说明 |
|------|------|
| `BSP_RELEASE_NOTES.md` | BSP 发布说明（占位模板） |
| `kernel-config/README.md` | 内核配置说明 |
| `kernel-config/kernel.config` | 设备运行态内核配置快照 |
| `defconfig/rk3288_defconfig` | 内核基准配置 |

## 根目录
| 文件 | 说明 |
|------|------|
| `AUDIT_REPORT.md` | 全项目审计报告（综合评分 2.9/5） |
| `CREDITS.md` | 完整依赖清单与模型台账 |
| `DEVELOP.md` | 系统架构、构建、测试、工作流规范 |
| `GITHUB_SCAN_AUDIT.md` | GitHub 安全与质量扫描审计 |
| `GOVERNANCE_VERIFICATION.md` | 架构治理落地验证报告 |
| `README.md` | 项目简介与快速开始 |
| `REMOTE_BRANCH_AUDIT.md` | 19→3 远程分支审计与清理 |
| `RK3288_CONSTRAINTS.md` | RK3288 目标设备画像与约束清单 |

> 以下研究与审计附录位于 `docs/research/` 下的独立文件（2026-07-05 从 [DEVELOP.md](../DEVELOP.md) 附录拆分为独立文档，DEVELOP.md 中仅保留指向这些文件的锚点）：
> - 附录 B：[加速方案可行性分析与实现状态](research/appendix-b.md)
> - 附录 C：[Android 摄像头调用机制研究](research/appendix-c.md)
> - 附录 D：[人脸识别技术实现方案研究](research/appendix-d.md)
> - 附录 E：[性能优化与故障排障研究](research/appendix-e.md)
> - 附录 F：[证据日志分析报告](research/appendix-f.md)
> - 附录 G：[稳定性治理审计修复计划](research/appendix-g.md)
> - 附录 H：[稳定性治理分阶段执行报告](research/appendix-h.md)（占位文档，内容未填充）
