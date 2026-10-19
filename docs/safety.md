<!-- DOCSYNC_DOC
type=process
status=living
as_of=2026-10-18
-->

# Safety — Loop Engineering

## Path Denylist

Never modify without explicit human approval:
- `.env`, `.env.*`
- `auth/`, `payments/`, `secrets/`, `credentials/`
- Any file containing API keys, tokens, or passwords
- CI/CD configuration (`.github/workflows/`)
- Branch protection rules / merge configuration

## 人工授权后的合并检查

权限由[loop约束](../loop-constraints.md)统一定义：禁止自动合并master，提交和推送也须分别取得用户确认。以下只是取得授权后的必要检查，不构成授权：

- 必需CI通过，审查意见处理完成。
- 使用squash and merge，标题遵循Conventional Commits。
- 不破坏测试；远端写入和删除分支仍需对应明确授权。

## MCP Scopes

| MCP Server | Allowed Tools | Risk Level |
|------------|--------------|------------|
| `fetch` | read-only URL content | low |
| `tavily-search` | web search | low |
| `firecrawl` | web search + scrape + crawl | medium |
| `exa-search` | AI search | low |
| `excel-mcp` | read-only Excel | low |
| `zotero` | reference management | low |
| `memory` | knowledge graph | low |
| `sequential-thinking` | reasoning | low |
| `ida-pro-mcp` | reverse engineering tools | medium |
| `desktop-commander` | terminal management | high |

## Sub-Agent Model Policy

- All sub-agents: `deepseekv4flash` only (`claude-sonnet-4-6` / `claude-haiku-4-5`)
- Verifier/checker roles: prefer `claude-haiku-4-5` for cost efficiency
- Implementer roles: use `claude-sonnet-4-6`
- Higher-tier models (opus, fable): prohibited in loops

## Incident Response

1. Loop detects anomaly → pause → log to `loop-run-log.md`
2. Escalate to STATE.md High Priority section
3. Human must clear before resume

## Kill Switch

- Issue/command: `loop-pause-all`
- Effect: all schedulers pause, no auto-actions
- Resume: human removes flag from STATE.md
