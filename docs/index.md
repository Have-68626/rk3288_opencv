# 文档索引

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
| `RK3288_CONSTRAINTS.md` | RK3288 目标设备画像与约束清单 |

> 以下内容已合并到 [DEVELOP.md](../DEVELOP.md) 附录中：
> - 附录 B：加速方案可行性分析与实现状态
> - 附录 C：Android 摄像头调用机制研究
> - 附录 D：人脸识别技术实现方案研究
> - 附录 E：性能优化与故障排障研究
> - 附录 F：证据日志分析报告
> - 附录 G：稳定性治理审计修复计划
> - 附录 H：稳定性治理分阶段执行报告
