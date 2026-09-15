# Loop Constraints — rk3288_opencv

> 本文件由 `loop-constraints` 技能在每次运行开始时读取，约束**具有强制力**，loop 必须遵守。
> 新增规则直接追加到本文件末尾。

## Push & Merge

- 不要在我确认前推送。提交信息须先展示给我，确认后才执行 `git commit`；**禁止自动 push**。
- **禁止自动合并到 `master`**（本仓库主分支是 `master`，不是 `main`）。
- 合并前必须满足（与 `CLAUDE.md` 的 PR 合并规则一致）：
  1. 逐条阅读并解决 PR 上的全部 review / issue comments；
  2. 解决所有 merge conflict；
  3. 等待 CI 全部通过；
  4. **仅使用 squash and merge**（禁止 merge commit 与 rebase merge）；
  5. 删除已合并的源分支（删除远程/本地分支属受控高危操作，需单独授权）。
- 不绕过分支保护规则；`--admin` 仅限紧急修复。
- 若先创建 PR，请创建 draft PR，待我复核后再标记 ready。

## Paths

- 未经人工批准不得修改：`.env`、`.env.*`、`auth/`、`payments/`、`secrets/`、`credentials/`。
- 未经人工批准不得修改基础设施配置（`.github/workflows/`、分支保护与合并配置）。
- 完整的高风险路径清单见 `docs/safety.md`。
- 其他 AI agent 工作目录（`.trae/specs/`、`.Jules/`、`.codegraph/`）是草稿历史，非权威文档，不作为事实依据。

## Code

- 提出修复前先运行相关测试；不得为了让 CI 变绿而禁用测试。
- 不得重构与本次修复无关的代码 —— 一次运行只做一件事。
- **复杂或高风险逻辑修改（多分支状态、并发、生命周期、持久化、权限安全、跨模块调用、公共 API）必须走 TDD**：先建立可复现的失败，再改代码。
- 每个条目最多尝试 3 次修复，超出后升级给我。
- 改动算法关键逻辑、JNI 边界或硬件差异处理时，**必须写中文注释**。
- 提交前必须通过文档门禁：`node scripts/docs-sync-audit.js --skip-links`（要求 high=0 且 medium=0）。

## Communication

- 动手前先告诉我要做什么。
- 未经我批准不得关闭 issue 或 PR。
- 高危/敏感/越权命令（`git push -f`、`rm -rf`、删除分支/worktree 等）需先报备「改动内容 + 风险 + 回滚方案」，并在 ask 窗口等我手动输入 **"仅本次临时授权"** 后，才执行**一次**。

## Budget

- token 消耗达到日上限 80% 时，切换为 report-only 模式。
- `loop-pause-all` 生效时立即退出。

---

## Models

- 所有子代理仅使用 `deepseekv4flash`（映射到 `claude-sonnet-4-6` 或 `claude-haiku-4-5`）。
- 任何子代理不得调用更高层级模型（opus、fable 等）。
- 子代理模型选择：verifier / checker 类角色优先 `claude-haiku-4-5`；implementer 类角色使用 `claude-sonnet-4-6`。
