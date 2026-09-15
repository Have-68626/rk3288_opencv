# Loop Configuration — rk3288_opencv（Minimal Triage）

## Active Loops

| Pattern | Cadence | Status | Command |
|---------|---------|--------|---------|
| Daily Triage | 1d | L1 report-only | `/loop 1d Run $loop-triage` |

## Human Gates

- L2 检查清单完成前不做自动修复（见 `docs/patterns/loop-design-checklist.md`）
- 高风险路径需人工复核（清单见 `docs/safety.md`）

## Worktrees

- L2+ 派发 implementer 子代理时使用 `isolation: worktree`。
- 每次修复尝试一个 worktree；verifier 判定 REJECT 后丢弃。

## Connectors (MCP)

- L1 report-only 模式下 MCP 可选。
- L2+ 需要 GitHub MCP 读取 CI / issue；在取得信任前，连接器范围限定为只读 + 评论。

## Budget

- 每次运行子代理上限：0（L1）
- 每日上限与超额处置见 `loop-budget.md`
- 每日复核 `STATE.md`

## Links

- Pattern: [daily-triage](docs/patterns/daily-triage.md)
- Checklist: [loop-design-checklist](docs/patterns/loop-design-checklist.md)
- Constraints: [loop-constraints](loop-constraints.md)
- Budget: [loop-budget](loop-budget.md)
- Run log: [loop-run-log](loop-run-log.md)
