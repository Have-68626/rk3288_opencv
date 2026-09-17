<!-- DOCSYNC_DOC
type=process
status=living
as_of=2026-09-17
-->

# Claude Code 权限自动审批：诊断、机制与方案

本文处理一件事：**在 Claude Desktop 接入第三方模型（cc-switch 网关）的前提下，如何把权限弹窗降到可接受水平，同时不放宽任何安全边界。**

它替代两份 AI 生成的工作区草稿——《Claude Desktop 接入第三方模型后的权限自动审批方案》v1.0 与《rk3288_opencv 项目权限自动化审批规则》v1.0。两者都未被版本控制、且含已被证伪的规格陈述（见 §5），**不得再作为事实来源**。

> **本轮交付边界**：只产出本文档，**未改动任何配置文件、未部署任何 hook 或服务**。文中所有配置片段均为**待实施的建议**。
>
> **版本锚**：Claude Code v2.1.272 · Claude Desktop（Code 标签页，`claude-desktop-3p`）· Windows 11 · cc-switch 本地网关 `127.0.0.1:15721`。本文的官方机制结论均以该版本为准，会随版本漂移。

---

## 0. 证据分级约定

**阅读本文时只信带标记的句子。** 三条同样重要：

| 标记 | 含义 |
|---|---|
| **【官方】** | 引自 `code.claude.com/docs` 原文，附 URL 与逐字引文，可作规格断言 |
| **【实测】** | 本机可复现的观察，附命令与观察点 |
| **【未核实】** | 推断、二手转述、或官方文档自相矛盾处 —— **不得当规格使用**，只能当待测项 |

**硬规矩**：任何"官方支持 / 官方不支持"的句子，没有 【官方】出处的一律降级为 【未核实】。前两份草稿的病根正是把自己的推断写成了规格。

---

## 1. 结论速览

1. **该用 hook 实现吗？** —— **是，用 `PermissionRequest` 事件。** 它是官方唯一能在**弹窗阶段**给出裁决的入口。但草稿给的输出 JSON **结构是错的**（§5 E1），照抄它写出的 hook 不生效且**静默**。
2. **能否另开一个会话来审批？** —— **不能，这条路被官方从三个角度各自独立关掉**（§8）。你要的"可监察会话"不能以"替我点同意"的形态存在；但有满足真实目的的三条替代路径。
3. **但在写任何代码之前，先看 §2 的实测数据**：你的问题**很可能不是 auto 模式失效**，而是**ask 规则过宽造成的弹窗风暴**。这两者的修法完全不同，且后者的成本低得多。

---

## 2. 实测现状：问题的真实规模

数据取自 `%LOCALAPPDATA%\Claude-3p\logs\main.log`（Desktop 应用日志）。**判定方式**：统计 `Emitted tool permission request` 行与 `decision=` 行。

| 指标 | 实测值 |
|---|---|
| 权限请求总数 | **1184** 次（9/15 = 392、9/16 = 533、**9/17 = 259**，当日数据截到 17:00） |
| 按工具 | **Bash 1089（92%）** · AskUserQuestion 26 · ExitPlanMode 8 |
| 决议分布 | **`decision=once` 1176 次** · `decision=deny` **2 次** · 永久放行 **0 次** |

**三条读数**：

- **问题是真的，且今天仍在发生**——9/17 一天 259 次弹窗，不是历史遗留。
- **零拒绝、零永久规则**：两天里你点了 1176 次"放行一次"，只拒绝过 2 次。这说明两件事：① 打断成本极高且几乎全是重复审批；② 实际执行过的命令整体可信，**规则误伤的基线风险很低**。
- **92% 集中在 Bash**，所以优化面是明确的。

> **必须知道的两个日志限制**（否则会像我一样差点用错方法）：
>
> 1. **该日志只记工具名与会话，不记命令内容**——原始行形如 `Emitted tool permission request <uuid> for Bash in session local_…`。因此**无法从日志反推是哪条规则命中的**，只能靠 §11 的行为探针。
> 2. **`%LOCALAPPDATA%\Claude-3p\logs\cli-diagnostics.jsonl` 不能用来验证工具期 hook。** 该文件确实记录 `hook_spawn_started` / `hook_spawn_completed` 并带 `hook_event_name`，但**全部 974 条只有 `SessionStart`(808) 与 `SessionEnd`(166)**（491+483=974，与字段出现次数精确吻合）；而本项目**已配置** `PreToolUse`/`PostToolUse`（graphify 守卫）却一条都没有。**结论：该日志只覆盖会话生命周期类 hook，工具期 hook 不在其中** —— 用"hook 事件是否出现在此日志"来判断 `PermissionRequest` 是否触发是**无效**的，必须改用 hook 自写审计日志作主验证（§11 E3）。

---

## 3. 先诊断：auto 模式到底有没有生效

用户级配置里**已经是** `permissions.defaultMode = "auto"`，且 `env` 有 `CLAUDE_CODE_ENABLE_AUTO_MODE=1`。所以正确的第一步**不是**造一个替代品，而是分清**弹窗属于哪一类**——因为官方语义下，有一类弹窗在 auto 模式下**必然出现，且是正确行为**。

### 3.1 三个候选病因（按可能性排序）

| # | 病因 | 判据 | 修法 |
|---|---|---|---|
| **(a)** | **ask 规则过宽**（**最可能**） | 官方：按命令内容匹配的 ask 规则在 auto 模式下仍回落弹窗（§4.7）。本项目 `settings.local.json` 有 **26 条 ask**，且含 `Bash(node:*)`、`Bash(npm:*)`、`Bash(python:*)`、`Bash(find:*)`、`Bash(mkdir:*)`、`Bash(cp:*)`、`Bash(mv:*)` 等极宽条目——它们覆盖了日常命令的大半 | **重排规则**（§6.2），不需要任何 LLM |
| **(b)** | **分类器请求失败** | 会有一条**点名某个模型**的消息，称 auto mode "cannot determine the safety" of an action【官方】。此时官方行为是**拒绝该动作且不给通知、不进 `/permissions` 的 Recently denied** | 先看是否出现该消息；出现则 hook 层是唯一可靠兜底 |
| **(c)** | **`autoMode.environment` 为空** | 本机 `claude auto-mode config` 输出的生效配置**只有 `allow` 与 `soft_deny` 两个键**，**没有 `environment`**【实测】。官方称 `environment` 是"绝大多数组织的主要字段"，分类器靠它判定什么算"外部目标" | 补齐 `autoMode.environment`（§6.3） |

> **结构性、不可修的弹窗**：1184 次里有 **26 次 AskUserQuestion + 8 次 ExitPlanMode**——这两类工具**在任何模式下都必须由人回答**（官方「actions no mode auto-approves」清单）。**别把她们当成故障去修。** 可优化的靶子是那 1089 次 Bash。

### 3.2 可执行的诊断命令

`claude auto-mode` 是官方自检子命令组【实测，本机 v2.1.272 可用】：

```
claude auto-mode config      # 打印当前生效配置；本机输出 = 出厂默认（只有 allow 与 soft_deny）
claude auto-mode defaults    # 打印内置默认规则
claude auto-mode critique    # 让模型评审你的自定义规则；顺带就是"辅助模型调用能否跑通"的探针
claude auto-mode reset       # 移除用户设置里的 autoMode 段，回到出厂默认
```

配合 §11 的 D1/D2 行为探针，可把病因收敛到 (a)/(b)/(c) 之一。

---

## 4. 官方机制事实基线

### 4.1 `PermissionRequest` 事件（本文的核心载体）

触发时机【官方】：**"When a tool call needs a permission decision"**——即权限弹窗即将出现时。

**输入 JSON**【官方】：

```json
{
  "session_id": "abc123",
  "transcript_path": ".../00893aaf-....jsonl",
  "cwd": "/Users/...",
  "permission_mode": "default",
  "hook_event_name": "PermissionRequest",
  "tool_name": "Bash",
  "tool_input": { "command": "rm -rf node_modules", "description": "Remove node_modules directory" },
  "permission_suggestions": [
    { "type": "addRules",
      "rules": [{ "toolName": "Bash", "ruleContent": "rm -rf node_modules" }],
      "behavior": "allow", "destination": "localSettings" }
  ]
}
```

**输出 JSON**【官方】——**注意嵌套层级，这是最容易写错的地方**：

```json
{
  "hookSpecificOutput": {
    "hookEventName": "PermissionRequest",
    "decision": {
      "behavior": "allow",
      "updatedInput": { },
      "updatedPermissions": [
        { "type": "addRules",
          "rules": [{ "toolName": "string", "ruleContent": "string" }],
          "behavior": "allow",
          "destination": "session",
          "mode": "default",
          "directories": ["string"] }
      ],
      "message": "string"
    },
    "updatedInput": { },
    "updatedPermissions": [ ],
    "message": "string",
    "interrupt": false
  }
}
```

- `updatedPermissions[].type` 取值：`addRules` · `replaceRules` · `removeRules` · `setMode` · `addDirectories` · `removeDirectories`
- `destination` 取值：`session` · `localSettings` · `projectSettings` · `userSettings`
- `mode` 取值：`default` · `auto` · `acceptEdits` · `dontAsk` · `bypassPermissions` · `plan` · `manual`
- `updatedInput` **只在 `allow` 时可用**，且替换的是**整个输入对象**（未改的字段也要带上）；**修改后的输入会重新过 deny 与 ask 规则**【官方】

**无输出时的语义**【官方】："If a hook exits with code 2 without a `decision` object, the permission flow proceeds unchanged. **If no hook returns a decision, and in sessions that cannot show a prompt, the tool call is denied.**"

→ 这一句是本文 **fail-closed 设计的官方依据**：无 decision 时，**能弹窗就弹窗，不能弹窗就拒绝**。两个方向都不是"放行"。

### 4.2 `behavior: "ask"` —— 强制人工的官方手段

【官方】：**"In auto mode, an `ask` from a hook forces a permission prompt, preventing silent approval by the classifier."** 弹窗会标注来源（如 `[settings]`、`[plugin:<name>]`、`[skill]`）。

→ 这正是用户红线（§9.1）的机器保证方式。

⚠️ **【未核实】**：输出 schema 的示例里 `decision.behavior` 只写了 `allow | deny`，而"hook 可返回 ask"的说明与 `updatedPermissions[].behavior` 里都有 `ask`。**`ask` 究竟写在 `decision.behavior` 还是别处，官方示例未给全** —— 必须实测（§11 E1-D）。

### 4.3 五种 handler 型（全部真实存在）

| type | 字段 | 默认超时 |
|---|---|---|
| `command` | `command`、`args`、`async`、`asyncRewake`、`shell`、`timeout`、`statusMessage`、`once` | **600s** |
| `http` | `url`、`headers`、`allowedEnvVars`、`timeout`、`statusMessage` | **600s** |
| `mcp_tool` | `server`、`tool`、`input`、`timeout`、`statusMessage` | **600s** |
| `prompt` | `prompt`、`model`、`timeout`、`continueOnBlock` | 30s |
| `agent` | `prompt`（`$ARGUMENTS` 占位 hook 输入）、`model`、`timeout` | 60s |

（`UserPromptSubmit` / `PreModelSwitch` / `PostModelSwitch` 为 30s，`MessageDisplay` 为 10s，`SessionEnd` 共享 1.5s 预算。）

> ⚠️ 默认 600s 意味着**不加 `timeout` 的 `command`/`http` hook 会把主会话挂住十分钟**。落地时必须显式设小。

### 4.4 `prompt` 与 `agent` 型（官方内建的 LLM 审查）

- **`prompt`**【官方】：用 LLM 判断是否放行，把 hook 输入 + 你给的 prompt 发给 Claude 模型（**默认 Haiku**），输出 `{"ok": true|false, "reason": "...", "impossible": true|false}`；**有 `model` 字段**。
- **`agent`**【官方】：与 `prompt` 类似但**提供多轮工具访问**——派生子代理，可用 Read/Grep/Glob 调查；输出 `{"ok": true}` 放行 / `{"ok": false, "reason": "..."}` 阻止；**有 `model` 字段**。
- **两者都支持 `PermissionRequest`**【官方】，支持清单为：`PermissionDenied` · `PermissionRequest` · `PostToolBatch` · `PostToolUse` · `PostToolUseFailure` · `PreToolUse` · `Stop` · `SubagentStop` · `TaskCompleted` · `TaskCreated` · `TeammateIdle` · `UserPromptExpansion` · `UserPromptSubmit`。

⚠️ **【未核实】**：`prompt`/`agent` 输出的是 `{"ok": …}`，而 `PermissionRequest` 期望 `hookSpecificOutput.decision.behavior`。**两套 schema 如何对接，官方没有说明** —— `{"ok": true}` 到底映射成 allow 还是 deny，必须实测（§11 E2）。

### 4.5 matcher 语义【官方】

| matcher | 语义 |
|---|---|
| `"*"`、`""`、省略 | 全匹配 |
| 只含字母、数字、`_`、`-`、空格、`,`、`\|` | 精确串，或用 `\|`/`,` 分隔的列表 |
| 含其他任何字符 | **JavaScript 正则，且不锚定** |

### 4.6 配置合并与回滚开关【官方·部分】

- **列表键是合并而非替换**："When you set the same list key, such as `permissions.allow`, in more than one file, Claude Code **combines the lists** instead of picking one."
- `disableAllHooks`（关掉 hooks、自定义 status line、自定义文件建议命令）与 `allowManagedHooksOnly`（只跑组织部署的 hooks）两个开关存在。
- ⚠️ **【未核实】**：hooks 条目在 user/project/local 之间的合并语义，我未取到逐字原文；有一处二手引文称 "Hook entries merge across settings levels rather than replacing each other"。**回滚方案必须按"合并"设计**（即用 `disableAllHooks: true`，而不是"给一份不含 hooks 的配置文件"）—— 后者在合并语义下**无效**。

### 4.7 规则求值顺序与 hook 的关系【官方】

- **求值顺序**："Rules are evaluated in order: **deny, then ask, then allow**. The first match in that order determines the outcome, and **rule specificity doesn't change the order**." 宽 deny 压过更精确的 allow；匹配的 ask 即使有更具体的 allow 也照样弹窗。
- **规则 vs hook**："Hook decisions don't bypass permission rules. Claude Code evaluates deny and ask rules regardless of what a **PreToolUse** hook returns…"
- ⚠️ **【未核实】该句的限定语是 `PreToolUse`。不得据此推断 `PermissionRequest` hook 的 allow 也压不过 ask 规则** —— 恰恰相反，后者存在的意义就是解决弹窗。**这是本方案最需要实测的一条**（§11 E1-B）。
- **auto 模式下**：按命令内容匹配的 ask 规则（如 `Bash(git push *)`）**"fall back to a permission prompt"**【官方】。受保护路径的写入**即使命中 allow 规则也路由到分类器**【官方】。

### 4.8 权限模式与 `autoMode` 配置块

- 模式合法值【官方】：`default` · `acceptEdits` · `plan` · `auto` · `dontAsk` · `bypassPermissions`。
- `permissions` schema：`{ defaultMode, allow[], ask[], deny[], additionalDirectories[], disableBypassPermissionsMode }`。
- `autoMode` 块【官方，`/docs/en/auto-mode-config`】：

```json
{ "autoMode": {
    "environment": ["$defaults", "Source control: ...", "Trusted internal domains: ..."],
    "allow":       ["$defaults", "..."],
    "soft_deny":   ["$defaults", "..."],
    "hard_deny":   ["$defaults", "..."],
    "classifyAllShell": true } }
```

  - `$defaults` = 在该数组内插入内置默认规则；**省略则整段默认规则被自定义内容替换**。
  - `classifyAllShell: true` = auto 模式下**暂停所有 Bash/PowerShell 的 allow 规则**，强制分类器评估每条 shell 命令（"trades latency for coverage"）。
  - 分类器内部优先级：`hard_deny`（无条件阻断）→ `soft_deny`（可被用户意图或 allow 覆盖）→ `allow`（作为 soft_deny 的例外）→ 用户显式意图。

### 4.9 auto 模式的 provider 覆盖（证伪草稿立论的关键）

【官方】：auto mode **对所有 provider 上的所有用户可用**，包括 Anthropic API、Claude Platform on AWS、Amazon Bedrock、Google Cloud's Agent Platform、Microsoft Foundry、signed-in Claude apps gateway；且逐字：

> **"It works with third-party providers, custom base URLs, Bedrock, Vertex, and Foundry."**

唯一例外限定句：**"It does not work with a custom `ANTHROPIC_BASE_URL` if that URL does not meet the provider requirements."** 模型要求：Anthropic API 上需 Opus 4.6+ / Sonnet 4.6+ / Fable；第三方 provider 上需 Sonnet 5 / Opus 4.7+ / Fable。`CLAUDE_CODE_ENABLE_AUTO_MODE=1` 的强制要求**已在 v2.1.207 移除**。

### 4.10 受保护路径与"任何模式都不自动批准"的清单【官方】

- **受保护目录**：`.git`、`.config/git`、`.vscode`、`.idea`、`.husky`、`.cargo`、`.devcontainer`、`.yarn`、`.mvn`、**`.claude`**（`.claude/worktrees` 除外）。
- **受保护文件**：`.gitconfig`、`.gitmodules`、`.bashrc` 系列、`.zshrc` 系列、`.profile`、`.envrc`、`.npmrc`、`.yarnrc*`、`bunfig.toml`、`.bazelrc` 系列、`.pre-commit-config.yaml`、`lefthook*`、`*-wrapper.properties`、`.devcontainer.json`、`.ripgreprc`、`pyrightconfig.json`、**`.mcp.json`**、**`.claude.json`**。
- **永不被自动批准（含 bypassPermissions 之外的任何模式）**：命中显式 ask 规则的工具；要求用户交互的工具（内置 `AskUserQuestion` 与标记 `requiresUserInteraction` 的 MCP 工具）；**`rm`/`rmdir` 打向 critical path**（"no allow rule or `PreToolUse` hook `"allow"` approves"）；跨会话 messaging 保护；工作目录外的读取（当 `permissions.blockReadsOutsideWorkingDirectories` 开启）。

> **由 4.10 推出的工程结论**：任何"让 hook 通过 `updatedPermissions` 自动写入 `.claude/settings.local.json`"的设计，**都会撞上人工确认**——`.claude` 是受保护目录。规则应当**直接写进文件**，不要指望 hook 自动沉淀。

### 4.11 `bypassPermissionsGateByAccount` 的真实归属

它**不是** Desktop 用户偏好，而是官方 **Managed settings（仅企业托管设置可设）** 一节的键【官方·归属已定位】。普通用户在用户级 settings 里设不了它。⚠️ 其确切语义未逐字核实【未核实】。

---

## 5. 勘误表：两份草稿的逐条判定

| # | 草稿陈述（出处） | 判定 | 正确表述 |
|---|---|---|---|
| **E1** | 输出 `{"decision":{"behavior":"allow"}}`（方案 §2.4、§4.2.4、§4.3） | **错·致命** | 必须包 `hookSpecificOutput` 并带 `hookEventName`（§4.1）。按草稿写会**完全不生效且静默**——无 decision 即回落正常弹窗，看不出坏没坏 |
| **E2** | "官方 auto …第三方模型接入场景下**不可用或不生效**"（方案 §1.1、§2.2、§4.1 注、§7） | **过时/已被推翻** | auto mode 覆盖所有 provider 与自定义 base URL（§4.9） |
| **E3** | `prompt` 型 hook "**固定调用 Claude 模型**，第三方接入不适用"（方案 §4.2.1） | **错** | `prompt` 与 `agent` 型**都有 `model` 字段**（§4.3） |
| **E4** | `PreToolUse` "**不能批准**（静默不视为批准）"（方案 §2.3） | **错** | `PreToolUse` 的 `permissionDecision` 可取 `allow`/`deny`/`ask`/`defer`；真正的官方约束是"hook 决策不绕过 deny/ask 规则"，且该句限定 PreToolUse |
| **E5** | hook 超时默认 "10–15s"（方案 §4.1、§4.3 的 `timeout` 值） | **错** | 默认 `command`/`http`/`mcp_tool` = **600s**、`prompt` = 30s、`agent` = 60s。草稿里的 10/15 是作者自选值，不是默认（§4.3） |
| **E6** | `bypassPermissionsGateByAccount` 归为 "Desktop preferences"（方案 §2.1 表） | **归属错** | 属 Managed settings（§4.11） |
| **E7** | 全文未提 cc-switch 会重写 `~/.claude/settings.json` | **致命遗漏** | 见 §6.4 —— 放错位置的配置会静默消失 |
| **E8** | 全文未提受保护路径 | **缺关键约束** | 除 bypassPermissions 外永不被自动批准；`.claude` 是受保护目录（§4.10） |
| **E9** | 全文未提 hooks 的合并语义与 `disableAllHooks` | **缺回滚与隔离手段** | 隔离/回滚须用 `disableAllHooks: true`（§4.6） |
| **E10** | 建议 `defaultMode: "default"`（方案 §4.1 注） | **与现状冲突且降低自动化** | 本机已是 `auto`；hook 层的身份是"补缺口"，不是"替代 auto" |
| **E11** | MCP 自动批准靠 `claude_desktop_config.json` 的 `autoApprove`/`alwaysAllow`（方案 §2.1 表） | **未核实/很可能不适用** | 该键在 hooks 与 permissions 文档中零命中；Claude Code 侧 MCP 授权走 permissions 规则 |
| **E12** | "hook 返回 allow **不会覆盖** deny/ask 规则——规则层仍先于 hook"（方案 §2.4） | **越界推广** | 官方该表述限定 `PreToolUse`（§4.7）；对 `PermissionRequest` **未核实**，须实测 |
| **E13** | "服务失败 = 弹窗兜底，**不会造成权限洞**"（方案 §5 回滚、§6.1） | **只说了一半** | 无 decision 时：能弹窗→弹窗，**不能弹窗→deny**（§4.1）。非交互场景下"兜底"的方向是**拒绝** |
| **E14** | 流程图暗示 hook 能看到"所有工具调用"（方案 §3） | **语义错** | `PermissionRequest` 只在**需要权限裁决时**触发；已被 allow 规则放行的调用**不会**触发 |
| **E15** | 规则文档把 `Bash(find *)`、`Bash(mkdir *)`、`Bash(cp *)` 等列为 allow（规则 §3.2） | **不建议照抄** | `Bash(... *)` 是命令前缀匹配，宽 allow 会把危险变体一并放行（如 `cp` 覆盖、`find -delete`）。**且草稿给的语法与官方 `Bash(rule:*)` 形式不一致**，须以官方为准 |

**判定为"真"、应予保留的**（别误删）：`PermissionRequest` 事件存在且**能批准**；五种 handler 型全部真实；`autoMode` 四键与 `$defaults` 真实存在；输入含 `permission_suggestions`（`addRules`/`behavior`/`destination`）——这一条当初最可疑，实为**真**。

---

## 6. 推荐架构

### 6.1 分层（第 0 层是官方 auto 模式本身）

```
工具调用
  │
  ▼
⓪ 官方 auto 模式（本机 defaultMode="auto"）—— 主力
  │   先按 §3 诊断它在本机是否真的生效
  ▼
① 权限规则层（确定性）  deny → ask → allow
  │   allow 命中 → 放行；deny 命中 → 拒绝；ask 命中 → 进入 ②
  ▼
② PermissionRequest hook 层（本轮新增）
  │   2a 确定性快速判定（本地、无模型调用）
  │      · 只读白名单 → allow
  │      · 红线清单 / 受保护路径 → **强制 ask，绝不 allow**
  │   2b 语义层（http + 本地服务 → 第三方 LLM）
  │      · 只处理 2a 未覆盖的复杂/嵌套/批量命令
  │   allow / deny / ask / 无输出（= 交回弹窗）
  ▼
③ 人工兜底：弹窗 + "仅本次临时授权"
```

**定位必须说清**：因为本机**已经是 auto**，②**不是"复刻 auto"，而是补 auto 的两个确定性缺口** ——（i）auto 对按内容匹配的 ask 规则一律回落弹窗，（ii）分类器失败时的静默拒绝。定位错了会重复造轮子并放大误放行风险。

### 6.2 先做、且收益最大的一步：重排规则（不需要任何 LLM）

**依据 §3.1 病因 (a)**：26 条宽 ask 规则是弹窗的主要来源。把它重排成"宽 allow / 窄 ask"即可消掉大部分弹窗：

- **allow**：只读与本地构建类，用官方 `Bash(前缀:*)` 形式，逐条窄化（不要用 `Bash(cp *)` 这种会连危险变体一起放行的写法）。
- **ask**：保留真正需要人的检查点 —— push/PR、发布、设备写操作（`adb shell am/pm/input/push/install`）、批量删除。
- **deny**：系统破坏类，并**把红线命令同时放进 deny**（规则层先于 hook，是兜底纵深，见 §9.1）。
- **注意**：`ask` 优先级高于 `allow` 且不受 hook 影响（§4.7），所以"想永久放行"只能从 ask 移走。

### 6.3 第二步：补齐 `autoMode.environment`

本机生效配置缺 `environment`【实测】。补上受信任的仓库、域名、内部服务，能减少分类器的保守误拦。**注意 `$defaults` 语义**：数组里带上 `$defaults` 才保留内置规则，否则整段被替换（§4.8）。

### 6.4 配置落点与"存活矩阵"（cc-switch 是最大运维风险）

**问题**：cc-switch 每次切换/启动都用其模板重写 `~/.claude/settings.json`，只写它认识的键。

⚠️ **两处来源冲突，必须如实记录**：
- 本机记忆（2026-08）称：cc-switch 重写时会**丢弃 `permissions`**；
- 2026-09-17 一次对模板的只读检查**未复现该结论**（提示模板可能已更新、现已包含 `permissions`）—— 但**该检查本轮未由我独立复核**，标【未核实】。

**结论：不得依赖任一口径。** 落点选择与每次切换后的**键集比对**才是可靠做法：

| 落点 | cc-switch 会动它吗 | 作用域 | 结论 |
|---|---|---|---|
| `~/.claude/settings.json` 的 `hooks` | **会**（模板只含它认识的键） | 全机 | **不可持久**，除非同步进 cc-switch 模板 |
| cc-switch 模板本身 | 它就是模板 | 全机 | 想全局生效就改这里 |
| `<项目>/.claude/settings.json` | 不会 | 本项目 | 可靠，**但入库**、影响协作者 |
| `<项目>/.claude/settings.local.json` | 不会 | 本项目 | **推荐落点**：可靠 + 不入库 |

> **顺带一条安全提醒**：有证据表明本地 cc-switch 的配置存储中含**明文凭据**（本轮未独立复核，**我未读取、也不会输出其值**）。因此 §9.3 的审查服务必须**明令禁止读取或打印该存储**，审计日志也必须脱敏。

### 6.5 只对 Claude 生效

- **天然 Claude-only**：hooks 只从 `~/.claude/settings.json` 与 `<项目>/.claude/settings.json` 读取；codex（`~/.codex/`）与 opencode（`~/.config/opencode/`）**不读这些文件**。**只要用 hook 实现，就自动只对 Claude 生效。**
- ⚠️ **有一个交叉污染通道**：本机 cc-switch 的技能同步设置会把 **Skill 形态**的东西推到 codex/opencode（且这两个 app 在其 `visibleApps` 中是开启的）。**因此本方案不得用 Skill 承载，只能用 hook 或 MCP server。**

---

## 7. Q1：该用 hook 实现吗 —— 是，`PermissionRequest`

1. **唯一入口**：它是官方唯一"在权限弹窗阶段给出裁决"的事件（§4.1）。`PreToolUse` 只能在上游拦，且解决不了"ask 规则仍会弹窗"这个真实痛点。
2. **五种 handler 自由选**（§4.3），`prompt`/`agent` 型还内建 LLM 且可指定模型（§4.4）。
3. **天然 Claude-only**（§6.5）。
4. **写对结构**：务必用 §4.1 的真实嵌套。**E1 那个错误会让整个实现静默失效。**
5. **`behavior: "ask"` 是红线机制的官方实现**（§4.2）。

---

## 8. Q2：能否另开一个会话来审批 —— 不能

### 8.1 官方从三个角度各自独立关掉了这条路【官方·逐字】

出处 `/docs/en/cross-session-messaging`：

> **"It can't approve anything"**: a message from another session never counts as your consent, so it can't answer a pending permission prompt on your behalf.
>
> **"It can't change configuration"**: Claude Code instructs the receiving Claude never to change permission settings, `CLAUDE.md`, or other configuration because another session asked.
>
> **"Permission prompts still fire"**: if acting on the message requires a permission the receiving session doesn't have, you see the same prompt you'd see for any other work.

**不是权限不足，是设计上就不认。** 而且工程上还有第二个死结：hook 需要**同步**拿到裁决，而跨会话消息是**投递到队列、无同步回执**的——要靠自建"邮箱文件 + 轮询"胶水，延迟高、易碎，且审查会话自身的权限构成新的递归问题。

**但你的真实目的可以达成，只是要拆成两个诉求分别满足。**

### 8.2 三种可行载体对比

| 载体 | 官方支持 | 延迟量级 | 可监察性 | 失败模式 |
|---|---|---|---|---|
| **`agent` 型 hook**（子代理**可读文件**、`model` 可指定、60s） | ✅ 明确支持 `PermissionRequest` | 一次子代理回合 | 只能落审计日志 | 超时 → 无 decision |
| **`prompt` 型 hook**（单轮、默认 Haiku、30s） | ✅ 明确支持 | 一次模型往返 | 只能落审计日志 | 同上 |
| **`--bg` 常驻会话 + `attach`** | ⚠️ 会话本身官方支持，但**驱动它做裁决无官方通道** | 高 | ✅ `claude attach` 随时可看 | 递归、上下文漂移、成本不可控 |

### 8.3 建议的拆解

- **"由另一个 Claude 实例来判断"** → 用 **`agent` 型 hook**：它可 Read/Grep/Glob 读项目上下文，最接近你想象的形态，且是官方决策路径，不涉及"跨会话同意"。
- **"随时能看它怎么判"** → **审计日志**（每次判定写脱敏 JSONL）。若还想有会话可 attach，可另起一个 `claude --bg` **旁路观察会话**只读审计日志 —— 但**它不参与裁决**，避免把"监察"变成"授权通道"。
- **"想人工介入某个决定"** → 让 hook 返回 **`behavior: "ask"`**（§4.2）。这才是"人可随时介入"的官方语义，不需要任何会话。
- **若坚持让常驻会话裁决**：唯一自建形态是 `command` 型 hook 写请求文件 → `--bg` 会话读文件写回建议 → hook 读建议。**不推荐**：非官方审批语义、递归风险真实（官方仓库有 hook 递归烧掉约 5000 万 input tokens 的 issue，且**官方无防递归机制**）、必须用 `--settings '{"disableAllHooks":true}'` 隔离。

---

## 9. 安全设计（硬性）

### 9.1 用户红线的机器保证

用户红线原文：**"高危/敏感/越权命令的临时授权机制：无论当前处于任何权限模式（含 auto、bypassPermissions），当确实需要执行高危/敏感/越权命令时，必须通过 ask 窗口用中文详细说明该命令的目的与影响，并要求用户在 ask 窗口手动输入「仅本次临时授权」并提交后，才允许执行一次。"**

设计保证：

1. **hook 对识别为红线/受保护路径/越权的请求，一律输出 `behavior: "ask"` 或不输出**（§4.2 的强制弹窗语义），**任何情况下不得输出 `allow`**。
2. **"一次"语义**：红线清单**禁止**使用 `updatedPermissions` 沉淀规则（既因 `.claude` 是受保护路径会触发人工确认，也因它会破坏"仅一次"）。
3. **纵深**：红线命令**同时**放入 `permissions.deny`（规则层先于 hook）。
4. **机制边界必须写明**：hook **不能**替 Claude 生成 ask 窗口里的中文说明——说明由 Claude 依规则写出，hook 只负责"**不把请求放行掉**"。
5. 红线清单的权威来源是**本仓库的 `safety.md`** 与项目记忆中的安全红线，**引用而不复制**（`docs/DOC_STANDARD.md` §3 单一事实源）。

### 9.2 fail-closed

服务/模型不可达、超时、解析失败、非预期输出 → **一律不输出 decision** → 能弹窗则弹窗，**不能弹窗则官方会 deny**（§4.1）。**禁止任何"异常时返回 allow"的代码路径。** 显式设小 `timeout`（如 30–60s），避免 600s 默认值把主会话挂住。

### 9.3 审计日志（脱敏）

每次判定追加一行 JSONL：`{ts, session_id, tool_name, decision, layer, reason, latency_ms}` + **脱敏后的** `tool_input`。

- **脱敏在写入前完成**（用户红线："任何日志、调试输出中必须自动对敏感字段（如密码、Token、手机号、邮箱）脱敏处理"）：掩码 `password`/`token`/`secret`/`api[-_]?key`、`Bearer …`、`AKIA…`、`ghp_…` 等形态。
- **明令禁止读取或打印本地 cc-switch 的配置存储**（§6.4）。
- 日志**不入库**（含命令原文，可能带敏感上下文）。

### 9.4 递归防护

hook 拉起的任何 Claude 进程：① 带 `--settings '{"disableAllHooks":true}'`（**不能用"给一份不含 hooks 的配置文件"**，合并语义下令其无效，§4.6）；② 设环境变量哨兵，hook 入口检测到即立即返回无 decision；③ 设调用深度与超时上限。**官方无防递归机制，必须自建。**

### 9.5 回滚

去掉对应 settings 里的 hook 条目 **或** 设 `disableAllHooks: true`（合并语义下这才是"一键停"）。改配置前**备份**用户级与项目级 settings（后者本机已有 `.bak-perm-*` 先例）。回滚后验证：所有弹窗恢复、无静默放行。

---

## 10. 落地阶段（本轮未执行）

| 阶段 | 动作 | 验证 |
|---|---|---|
| P0 备份 | 复制用户级与项目级 settings 到 `.bak-<日期>` | 备份文件存在 |
| P1 诊断 | 跑 §3.2 与 §11 的 D1/D2 | 病因收敛到 (a)/(b)/(c) |
| P2 规则层 | 只改 allow/ask/deny，**暂不挂 hook** | 只读/构建无弹窗；push/设备写弹窗；破坏性命令被 deny |
| P3 规则重排 | 按 §6.2 重排 26 条 ask | 弹窗数下降，无误伤 |
| P4 补 `environment` | 按 §6.3 | 分类器误拦减少 |
| P5 小流量 hook | 挂 `PermissionRequest`，**只开"强制弹窗"分支，不做任何 allow** | 审计日志中**没有一条被误放行** |
| P6 放开 2a allow | 逐条把稳定白名单切到自动放行 | 每批观察，比对弹窗数 |
| P7 接入 2b 语义层 | http + 本地服务 | 用嵌套/批量命令验证 |
| R 回滚 | `disableAllHooks: true` + 还原 settings | 弹窗全部恢复 |

---

## 11. 验证方案（待落地时执行）

### D1 诊断：路由与分类器【实测·只读】

```
claude auto-mode config                                                   # 看生效配置（本机缺 environment）
grep -c "Emitted tool permission request" "%LOCALAPPDATA%\Claude-3p\logs\main.log"   # 弹窗计数基线
grep -oE 'decision=[a-z]+' "%LOCALAPPDATA%\Claude-3p\logs\main.log" | sort | uniq -c
```

观察点：`claude auto-mode config` 的键集；是否出现**点名某个模型**、说 auto mode "cannot determine the safety" 的消息；弹窗计数的日增幅。

### D2 行为探针

跑一条**不被 allow 命中、也非受保护路径**的只读命令。预期：auto 生效 → 无弹窗；分类器失败 → 弹窗或出现 D1 的失败消息。

### E1 `PermissionRequest` hook 的三态【核心】

- 用一条**落在 ask 规则里的无害命令**（如 `Bash(find:*)` 命中的 `find . -maxdepth 1`）。
- **E1-A（基线）**：无 hook → 预期弹窗。
- **E1-B（能否压过 ask）**：hook 返回 `decision.behavior: "allow"` → **观察弹窗是否消失**。消失即证明 §4.7 那条 PreToolUse 限制**不适用于** `PermissionRequest`；仍弹窗则说明 ask 规则独立求值。
- **E1-C（deny）**：对一条本该 allow 的命令返回 deny → 观察是否被拒、`message` 是否回传。
- **E1-D（ask 的表达位置）**：先试 `decision.behavior: "ask"`，不行再试 `updatedPermissions[].behavior: "ask"` → 观察是否**强制弹窗**、提示是否标来源。用于消解 §4.2 的【未核实】。

### E2 `prompt`/`agent` 型输出映射【官方歧义，必测】

配一个恒返回 `{"ok": true}` 的 `prompt` 型 hook，`model` 指向网关可解析的 id；对一条默认会弹窗的命令触发。观察结果是**自动放行 / 直接拒绝 / 弹窗**三者之一。再测 `{"ok": false, "reason": "…"}` 是否 deny 且 reason 回传。**用于消解 §4.4 的【未核实】。**

### E3 hook 是否在 Claude Desktop 触发

**不要用 `cli-diagnostics.jsonl`**（§2 已证其只记 `SessionStart`/`SessionEnd`）。**改用 hook 自身写审计日志**：挂上 hook 后在 Desktop 触发一次弹窗，检查审计日志是否新增该次判定记录；并用 `hook_spawn_completed` 观察异常退出。

---

## 12. 未核实项（不得当结论使用）

1. `prompt`/`agent` 型 hook 的 `{"ok": …}` 如何映射到 `PermissionRequest` 的 `decision.behavior`，映射方向如何（§4.4，E2）。
2. "hook 决策不绕过 deny/ask 规则"是否**也**适用于 `PermissionRequest`（官方原文限定 `PreToolUse`）（§4.7，E1-B）。
3. `behavior: "ask"` 的确切书写位置（§4.2，E1-D）。
4. hooks 条目在 user/project/local 之间的合并语义（§4.6）—— 有一处二手引文，未取到逐字原文。
5. `bypassPermissionsGateByAccount` 的确切语义（§4.11）。
6. cc-switch 是否仍丢弃 `permissions`（§6.4）—— 记忆与一次模板检查结论冲突，本轮未独立复核。
7. 本机第三方网关是否满足 auto mode 的 provider / 模型要求（§4.9，D1/D2 判定）。
8. hook 在 Desktop / CLI / IDE 三端是否都触发（E3 判定）。
9. 本地 cc-switch 配置存储是否确含明文凭据 —— 本轮**未读取**，仅作警示（§6.4）。

---

## 13. 证据来源

**官方文档**（2026-09-17 抓取）：

- `https://code.claude.com/docs/en/hooks` —— 事件全集、`PermissionRequest` 输入/输出、五种 handler 字段与默认超时、matcher 语义、无输出语义、`ask` 强制弹窗、settings 合并
- `https://code.claude.com/docs/en/permissions` —— 规则求值顺序、规则与 hook 的关系、模式取值、`permissions` schema、`bypassPermissionsGateByAccount` 归属
- `https://code.claude.com/docs/en/permission-modes` —— auto 模式的 provider/模型要求、受保护路径、"actions no mode auto-approves"、分类器评估顺序与失败行为、`disableAutoMode`
- `https://code.claude.com/docs/en/auto-mode-config` —— `autoMode` 四键、`$defaults`、`classifyAllShell`、分类器内部优先级
- `https://code.claude.com/docs/en/cross-session-messaging` —— §8.1 的三条逐字否决

**本机实测**（2026-09-17）：

- `claude --version` = 2.1.272；`claude auto-mode config|defaults|critique|reset` 子命令可用
- `~/.claude/settings.json`、`<项目>/.claude/settings.json`、`<项目>/.claude/settings.local.json` 的权限与 hooks 现状
- `%LOCALAPPDATA%\Claude-3p\logs\main.log` —— §2 的全部计数
- `%LOCALAPPDATA%\Claude-3p\logs\cli-diagnostics.jsonl` —— §2 的 hook 日志限制

**被本文取代的工作区草稿**（未版本控制、不在本仓库内，故不给可点击路径）：`Claude-Desktop-第三方模型-权限自动审批方案.md` v1.0（2026-09-15）、`rk3288_opencv-权限自动化审批规则.md` v1.0（2026-09-16）。两者同目录，另有两份开源对标调研报告（与本主题无关）。

**相关文档**：`safety.md`（本项目安全与高风险路径清单，红线权威来源）· `DOC_STANDARD.md`（本文档所遵循的规范）
