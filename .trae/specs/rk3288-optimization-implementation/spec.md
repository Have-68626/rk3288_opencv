# [RK3288 优化实施] Spec

## Why
[根据 `ANALYSIS_REPORT.md` 和 `RK3288_CONSTRAINTS.md` 的分析结果，项目中存在安全风险（数据备份开启、日志潜在泄露）、质量问题（硬编码路径、忽略异常）以及性能优化空间。需要针对 RK3288 平台特性进行针对性优化，同时遵守无 NPU 的硬件约束。]

## What Changes
- [**安全加固**: 修改 `AndroidManifest.xml`，禁用 `android:allowBackup`。]
- [**代码质量**: 修复 `MainActivity.java` 中的异常静默处理，添加日志记录。]
- [**构建优化**: 修改 `scripts/build_android.bat`，移除硬编码的绝对路径，改为依赖环境变量或相对路径。]
- [**日志优化**: 在 `AppLog.java` 中引入 `SensitiveDataUtil`，确保在非 DEBUG 模式下或写入磁盘时对敏感数据进行脱敏（遵循个人开发免责声明的前提下，提供脱敏能力）。]
- [**性能验证**: 确认 `CMakeLists.txt` 中已启用 NEON 指令集优化（`armeabi-v7a`）。]

## Impact
- Affected specs: [无]
- Affected code: [`app/src/main/AndroidManifest.xml`, `src/java/.../MainActivity.java`, `scripts/build_android.bat`, `src/java/.../AppLog.java`]

## ADDED Requirements
### Requirement: 安全配置
系统 SHALL 禁用 Android 数据备份功能，防止敏感数据被轻易导出。

### Requirement: 构建脚本可移植性
构建脚本 SHALL 不包含特定开发者的本地绝对路径，支持通过环境变量配置依赖路径。

## MODIFIED Requirements
### Requirement: 异常处理
[所有捕获的异常必须至少记录日志，严禁静默吞没异常。]

## REMOVED Requirements
### Requirement: 硬编码路径
**Reason**: [影响多人协作和 CI/CD 环境]
**Migration**: [使用环境变量或相对路径]
