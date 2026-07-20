# GitHub 远程分支审计报告

**日期**: 2026-07-06
**统计**: 共 19 个远程分支（含 master）

---

## 1. 活跃分支（需保留）

| 分支 | 说明 | 操作 |
|------|------|------|
| `master` | 默认分支 | — |
| `palette/fix-switch-accessibility-16194329876251733531` | **PR #441 开放中** | 待合入 |
| `palette-fix-switch-labels-12815923954358642341` | 有本地 worktree `hopeful-tharp-48ae96` | 保留 |
| `claude/serene-chaplygin-ea8365` | 有本地 worktree（当前会话） | 保留 |

---

## 2. 已合入但未清理的远程分支（建议删除）

| 分支 | PR | 状态 |
|------|-------|--------|
| `fix/gemini-audit-null-guards` | #429 | ✅ 已合并 |
| `sentinel/http-faces-server-dos-fix-12389990758656690515` | #426, #427 | ✅ 已合并 |
| `palette-fix-switch-labels-12815923954358642341` | #428 | ✅ 已合并（有本地 worktree 待处理） |
| `claude/intelligent-solomon-996ca3` | #430 | ✅ 已合并 |
| `bolt-optimize-engine-csv-formatting-6992114841700640310` | #408 | ✅ 已合并 |
| `palette-add-action-icons-4646665983791963895` | #354 | ✅ 已合并 |
| `fix-gemini-444` | #445 | ✅ 已合并 |
| `doc-fixes-audit-2026` | #446 | ✅ 已合并 |

---

## 3. 无关联 PR 的残留分支（AI 代理残留，建议删除）

这些分支由 Bolt/Jules AI 代理创建，无 PR 关联，内容已被后续治理批次覆盖。

| 分支 | commits ahead | 内容 | 建议 |
|------|---------------|------|------|
| `bolt-optimize-file-reading-9826675095701098073` | 1 | HttpFacesServer 文件读取优化（2 文件） | 删除 |
| `bolt/remove-redundant-clone-http-server-3107611620963044373` | 1 | MJPEG 流移除冗余 clone（2 文件） | 删除 |
| `palette-ux-enrollment-input-15078265720878684381` | 1 | PreviewPage Enter 键注册 | 删除 |
| `palette-ux-input-enter-13319731204000583563` | 3 | Jules 代理的 PreviewPage 修改 | 删除 |
| `palette-preview-input-accessibility-4212070798889607448` | 1 | 预览输入框可访问性 | 删除 |
| `sentinel-fix-unmasked-sensitive-logs-7915475011236300964` | 1 | 诊断日志敏感数据泄漏修复（2 文件） | 删除 |
| `sentinel/fix-global-log-masking-13641073004265058219` | 2 | 磁盘日志敏感数据泄漏修复（7 文件） | 删除 |

---

## 4. 分支处理优先级

| 优先级 | 分支数 | 说明 |
|--------|--------|------|
| P0（立即删除） | **7** | 无 PR 关联的 AI 代理残留分支 |
| P1（已合入） | **8** | PR 已合并，远程分支可安全删除 |
| P2（保留） | **3** | 开放 PR + 活跃 worktrees |

---

## 建议行动

1. **立即删除** 第三类（7 个 AI 残留分支）
2. **删除** 第二类中已清理 worktree 的 6 个分支（fix/gemini-audit-null-guards 等）
3. **保留** palette/fix-switch-accessibility（PR #441 开放中）和活跃 worktrees
