# [RK3288 适配与项目整理] Spec

## Why
[为了适配 RK3288 平台（ARM Cortex-A17 CPU，Mali-T764 GPU，无 NPU）的硬件特性，需要对项目进行针对性适配，并按照标准化目录结构整理代码库，以提升项目的可维护性和可移植性。]

## What Changes
- [建立《RK3288适配约束清单》（`docs/RK3288_CONSTRAINTS.md`），明确不支持的硬件特性。]
- [更新日志策略：DEBUG/VERBOSE 级别输出详细调试信息，ERROR/WARN 级别保持简洁。]
- [更新 README.md，增加“日志免责声明”。]
- [重构项目目录结构：]
    - `docs/`：存放芯片资料、约束清单。
    - `src/`：存放所有源码（按 cpu/gpu/common 分类）。
    - `scripts/`：存放编译、打包脚本。
    - `tests/`：存放测试代码及报告。
    - `tools/`：存放调试工具。
- [清理冗余文件（.tmp, .log, build/, .vscode/）。]
- [创建/更新 `.gitignore`。]
- [输出《项目整理报告》（Markdown 格式，可导出为 PDF）。]

## Impact
- Affected specs: [无直接受影响的功能规格说明书]
- Affected code: [整个代码库目录结构调整，CMakeLists.txt 及 build.gradle 配置更新，日志相关代码调整]

## ADDED Requirements
### Requirement: RK3288 硬件适配
系统 SHALL 仅使用 CPU（NEON）或 GPU（OpenCL/OpenGL ES）进行计算，严禁调用 NPU 相关 API。
#### Scenario: 硬件约束检查
- **WHEN** 开发者查阅文档或代码
- **THEN** 明确看到 RK3288 的硬件限制说明及相关配置

### Requirement: 标准化目录结构
项目 SHALL 遵循统一的目录结构规范。
#### Scenario: 代码组织
- **WHEN** 查看项目根目录
- **THEN** 看到 docs, src, scripts, tests, tools 等标准目录

## MODIFIED Requirements
### Requirement: 日志策略
[日志输出需根据级别区分详细程度，且在 README 中声明隐私风险。]

## REMOVED Requirements
### Requirement: 冗余文件与非标准目录
**Reason**: [清理项目，提升整洁度]
**Migration**: [迁移至标准目录或直接删除]
