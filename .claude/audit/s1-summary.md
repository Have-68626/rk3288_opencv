# S1 全局扫描 — 审计热度地图

**日期**: 2026-07-04 | **状态**: ✅ 已完成

---

## 产出清单

| 报告 | 文件 | 负责人 | 核心发现摘要 |
|:----|:----|:------|:------------|
| 📐 P1.1 架构全景 | [p1.1-architecture-scan.md](p1.1-architecture-scan.md) | Explore agent | 无库封装、误放跨平台文件、10 项架构问题 |
| 📊 P1.2 Git 模式 | [p1.2-git-analysis.md](p1.2-git-analysis.md) | Explore agent | 唯一贡献者 1 人、AI 提交占 40.7%、分支膨胀(430)、热点 Top 20 |
| 🔒 P1.3 安全枚举 | [p1.3-security-scan.md](p1.3-security-scan.md) | Explore agent | 21 项发现（高危 2）、civetweb 为风险核心、Android 安全实践良好 |
| 📏 P1.4 度量采集 | [p1.4-metrics.md](p1.4-metrics.md) | Explore agent | 230 源文件/43,790 行、23 超长函数、注释 1-4%、13 文件高复杂度 |

---

## 关键数字摘要

| 指标 | 值 |
|:----|:---|
| 总源文件数 | 230 |
| 总代码行数 | ~43,790 |
| C++ 占比 | 52.9% |
| 总提交数 | 1,112 |
| 唯一人类贡献者 | 1 人 |
| AI 提交占比 | 40.7% |
| AI 代码行占比 | 30.1% |
| 远程分支数 | 415 |
| 安全发现（高危） | 21 项（2 项高危） |
| 超长函数（>100行） | 23 个 |
| 注释率（核心模块） | 1-4% |
| 高复杂度文件（>50） | 13 个 |

---

## 待确认：是否启动 S2？

S2 将基于此热度地图，启动 7 个 subagent 对以下模块逐文件深入审查：

1. **C++ 核心引擎** (~50 文件) — Engine、VideoManager、BioAuth、FaceInferencePipeline
2. **Windows 服务** (~30 文件) — FramePipeline、HttpFacesServer、WinJsonConfig、D3D11Renderer
3. **Android 层** (~49 文件) — Camera2/CameraX、JNI 桥接、加密存储、权限状态机
4. **Web SPA** (~19 文件) — PreviewPage、API 层、状态管理
5. **构建/CI** (~15 文件) — CMake、Gradle、CI 配置
6. **测试覆盖** (~10 文件) — 自定义框架评估、覆盖矩阵
7. **文档合规** (~25 文件) — LICENSE、API 同步

**预计耗时**: 40-60 分钟

请回复 **"启动 S2"** 以继续。
