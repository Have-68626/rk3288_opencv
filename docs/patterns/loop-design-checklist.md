<!-- DOCSYNC_DOC
type=process
status=living
as_of=2026-08-13
-->

# Loop 设计检查清单

**适用对象**: 新增或调整 Claude Code Loop 配置时逐项核对

---

## 设计前确认

- [ ] 明确该 Loop 要解决的目标（巡检 / 执行计划 / 定时任务）。
- [ ] 明确节奏（`/loop <interval>` 间隔）与级别（L1 只读 / L2+ 可执行）。

## Active Loops 配置

- [ ] 在 LOOP.md Active Loops 表登记：pattern / cadence / status / command 四列。
- [ ] command 引用已安装的 skill（如 `Run $loop-triage`），确认 skill 存在。

## Human Gates（人工门）

- [ ] L2 检查清单完成前禁止自动修复（No auto-fix until L2 checklist complete）。
- [ ] 所有高风险路径标记人工审批，参考 `docs/safety.md` denylist。

## Worktrees（工作树隔离）

- [ ] L2+ 实现使用 `isolation: worktree` 隔离。
- [ ] 一次修复一个 worktree；verifier REJECT 后丢弃重建。

## Connectors（MCP 连接器）

- [ ] L1 report-only 可省略 MCP；L2+ 需要时限定 read + comment 只读权限。

## Budget（预算）

- [ ] 设置每轮 sub-agent 派生上限，避免失控扩展。
- [ ] 定期 review STATE.md 汇总状态。

## 回归

- [ ] 每次运行写入 `loop-run-log.md`，供 `loop-budget` 核对每轮开销。
