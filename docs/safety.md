# Safety — Loop Engineering

## Path Denylist

Never modify without explicit human approval:
- `.env`, `.env.*`
- `auth/`, `payments/`, `secrets/`, `credentials/`
- Any file containing API keys, tokens, or passwords
- CI/CD configuration (`.github/workflows/`)
- Branch protection rules / merge configuration

## Auto-Merge Allowlist

Auto-merge is allowed only when ALL conditions met:
- PR passes all required CI checks
- All review comments resolved
- Only squash merge (no merge commit, no rebase)
- Title follows Conventional Commits format
- Not breaking existing test suites

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
