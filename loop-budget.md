<!-- DOCSYNC_DOC
type=process
status=living
as_of=2026-09-15
-->

# Loop Budget — rk3288_opencv

> Primary loop: **Daily Triage**（配置见 `LOOP.md`，约束见 `loop-constraints.md`）

## Daily limits

> 下表为当前配置。数值沿用 loop 脚手架默认值，尚未按本项目实际用量校准 —— 如需调整，直接改本表。

| Loop | Max runs/day | Max tokens/day | Max sub-agent spawns/run |
|------|--------------|----------------|--------------------------|
| Daily Triage | 2 | 200M | 0 (L1) / 2 (L2) |

## On budget exceed

1. Pause schedulers（禁用 scheduled tasks / automations）
2. Append event to `loop-run-log.md`
3. Notify human（更新 `STATE.md` 的 High Priority 段）

## Kill switch

- 触发标记：`loop-pause-all`
- 只有人工在 `STATE.md` 中清除该标记后方可恢复

## Estimate spend

```bash
npx @cobusgreyling/loop-cost --pattern daily-triage
```
