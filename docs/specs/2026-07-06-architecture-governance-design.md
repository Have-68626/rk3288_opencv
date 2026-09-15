<!-- DOCSYNC_DOC
type=design
status=snapshot
as_of=2026-07-06
-->

# 架构治理设计文档

**版本**: v1.0
**状态**: 已落地 — 五条契约与三层实施路径由 ARCHITECTURE.md 及治理执行记录验证

---

## 1. 背景与目标

本设计文档定义跨子系统的**架构治理**规则与实施路径，用于治理项目未修复的技术债务，并统一
Core Engine / Windows Service / Android JNI 三侧 C++ 代码的架构风格。

治理范围聚焦 6 项技术债务：

1. JNI 侧裸 lock/unlock 资源配对（无 RAII 封装）。
2. 高频 JNI 回调无节流，可能冲刷 Java 层。
3. `native-lib.cpp` 单文件过大，JNI 函数无领域分组。
4. 配置存在 INI 与 JSON 双源真相。
5. Web SPA 缺少单元测试基础。
6. 主 `CMakeLists.txt` 超过千行，缺少模块化与编译器契约。

## 2. 设计原则：五条架构契约

跨子系统所有 C++ 代码必须遵守以下 5 条契约：

| # | 契约 | 核心要求 |
|:-:|:-----|:---------|
| 1 | RAII 资源封装 | lock/unlock、open/close、allocate/free 配对必须封装为 RAII 类型 |
| 2 | 纯函数管线 | 计算路径只做纯函数式变换，状态通过显式参数传递 |
| 3 | 原子状态提交 | 状态更新必须原子提交，禁止部分更新被外部观测 |
| 4 | 错误通道 | 帧路径禁止 throw，错误通过返回值 / 错误码显式传递 |
| 5 | 接口隔离与符号隐藏 | 通过接口与隐藏符号隔离各子系统实现细节 |

### 2.1 契约细则

**契约 1 — RAII 资源封装**

- 正例：Windows `SocketGuard` 析构自动 `closesocket`。
- 反例：`ANativeWindow_lock` / `ANativeWindow_unlockAndPost` 裸配对。
- 检查方法：`scripts/check-raii-violations.sh` 扫描裸 `_lock(` 调用。

**契约 4 — 错误通道**

- 帧路径（detect → recognize → result）禁止抛出异常，失败统一返回错误码并输出原因。
- 示例：加速契约使用 `requested` / `effective` / `evidence` / `reason` 四元组输出结果。

## 3. 分层实施结构

自顶向下分三层落地：

- **Phase 1 架构基线**：产出 `docs/architecture/ARCHITECTURE.md`（5 条契约 + 违规清单 +
  自动化检查），作为所有子系统的架构规约文档。
- **Phase 2 模式对齐**：将 Windows 侧已验证的 4 个设计模式移植到 Core / Android JNI：
  - `ScopedWindowLock` / `ScopedBitmapLock`（RAII 资源封装）。
  - `JniCallbackThrottle`（回调节流，源模式 ConnectionQuota）。
  - `JniMethodRegistry`（方法注册表，源模式 EndpointRegistry）。
  - `ConfigValidator` 通用化（错误通道）。
- **Phase 3 实现填补**：补齐 4 个具体问题 — Java INI 双源移除、Web 单元测试基础、
  CMake 模块化拆分、C++ 编译器契约（`-fno-exceptions -fno-rtti -fvisibility=hidden`）。

## 4. 关联文档

| 文档 | 角色 |
|:-----|:-----|
| [docs/architecture/ARCHITECTURE.md](../architecture/ARCHITECTURE.md) | 5 条契约的落地产物（依据本设计生成） |
| [docs/plans/2026-07-06-architecture-governance-plan.md](../plans/2026-07-06-architecture-governance-plan.md) | 本设计的实施计划（9 个 Task 按 Phase 执行） |
| `GOVERNANCE_VERIFICATION.md` | 治理落地验证报告（42/42 项通过） |

## 5. 验收标准

- 5 条契约在 ARCHITECTURE.md 中成文并有自动化检查脚本。
- Core / Windows / Android JNI 三侧通过 Phase 2 模式对齐消除裸资源配对。
- CI 6 个 job 保持通过，主 `CMakeLists.txt` 缩减至编排规模。
