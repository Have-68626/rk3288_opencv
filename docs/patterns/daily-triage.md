<!-- DOCSYNC_DOC
type=process
status=living
as_of=2026-08-13
-->

# Pattern：Daily Triage（每日巡检）

**所属系统**: Claude Code Loop（ralph-loop）

---

## 概述

Daily Triage 是面向仓库日常健康度的 L1 只读巡检模式：按固定节奏（默认 1 天）运行一次
`loop-triage`，把「近期变更、CI 失败、issues、对话结论」整理成可执行的发现报告，供主会话
决策，**不自动修复任何内容**。

## 触发方式

| 项 | 值 |
|:---|:---|
| 节奏 | 1d |
| 级别 | L1 report-only |
| 命令 | `/loop 1d Run $loop-triage` |
| 状态 | LOOP.md Active Loops 表登记 |

## 职责边界

- **做**：
  - 巡检最近一个周期内的代码变更（working tree / commits）。
  - 汇总 CI 失败与阻塞项。
  - 列出未处理的 issues / 对话结论。
  - 输出结构化发现报告（结论、证据、建议动作）。
- **不做**：
  - 不自动修 bug、不自动 merge、不改变配置。
  - 高风险路径一律交由人工审批（见 LOOP.md Human Gates）。

## 约束

- 读取仓库状态时遵循 `loop-constraints.md` 的强制约束。
- 每轮 token / 时间预算由 `loop-budget.md` 控制，超出即早退。
- 运行结果写入 `loop-run-log.md`，供后续回归审计。
