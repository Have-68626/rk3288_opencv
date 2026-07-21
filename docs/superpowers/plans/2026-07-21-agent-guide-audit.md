# Agent Guide Audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 AGENTS.md 和 CLAUDE.md 的关键运行事实纳入 docs-sync-audit，并阻断两份指南与仓库配置漂移。

**Architecture:** AGENTS.md 与 CLAUDE.md 使用相同的事实注释和共享区间标记。审计脚本从 Gradle、CI 与测试入口提取事实，验证两份指南的值和共享正文哈希；现有 docs-audit job 通过高优先级缺陷自动阻断。

**Tech Stack:** Node.js 内置 `node:test`、CommonJS、Markdown、GitHub Actions YAML 文本。

---

### Task 1: 为指南审计写失败测试

**Files:**
- Create: `tests/scripts/docs-sync-audit.test.js`

- [x] **Step 1: 写入测试夹具和失败断言**

```js
const { auditAgentGuides } = require("../../scripts/docs-sync-audit.js");

test("检测 CLAUDE.md 的事实漂移", () => {
  const result = auditAgentGuides(fixtureRoot);
  assert.ok(result.defects.some((d) => d.rule === "agent_guide_fact_drift"));
});
```

- [x] **Step 2: 运行测试，确认因缺少导出而失败**

Run: `node --test tests/scripts/docs-sync-audit.test.js`

Expected: FAIL，错误表明 `auditAgentGuides` 尚未定义。

### Task 2: 实现最小审计接口

**Files:**
- Modify: `scripts/docs-sync-audit.js`
- Test: `tests/scripts/docs-sync-audit.test.js`

- [x] **Step 1: 提取指南事实与共享正文**

```js
function parseAgentGuideFacts(md) { /* 读取 DOCSYNC_AGENT_GUIDE_FACTS 注释中的 key=value */ }
function extractSharedGuide(md) { /* 读取 DOCSYNC_SHARED_GUIDE_START/END 之间的正文 */ }
```

- [x] **Step 2: 从真实配置提取预期事实**

```js
const expected = {
  gradle_wrapper: "gradle-9.0",
  android_ci_java: "21",
  windows_ci_events: "pull_request,push",
  windows_ci_workflow_dispatch: "false",
  test_frameworks: "custom_bool,googletest",
  merge_strategy: "squash_and_merge",
};
```

- [x] **Step 3: 返回高优先级缺陷并导出接口**

```js
module.exports = { auditAgentGuides };
if (require.main === module) main().catch(/* 保持原退出码行为 */);
```

- [x] **Step 4: 运行单元测试，确认通过**

Run: `node --test tests/scripts/docs-sync-audit.test.js`

Expected: PASS。

### Task 3: 同步两份指南并接入主审计报告

**Files:**
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`
- Modify: `scripts/docs-sync-audit.js`

- [x] **Step 1: 在两份指南中加入相同事实注释和共享区间标记**

```md
<!-- DOCSYNC_AGENT_GUIDE_FACTS
gradle_wrapper=gradle-9.0
android_ci_java=21
windows_ci_events=pull_request,push
windows_ci_workflow_dispatch=false
test_frameworks=custom_bool,googletest
merge_strategy=squash_and_merge
-->
```

- [x] **Step 2: 同步 CLAUDE.md 的重复运行规则并保留其 graphify 专属章节**

- [x] **Step 3: 将 `auditAgentGuides()` 的缺陷合并到 `report.defects`**

- [x] **Step 4: 运行完整审计**

Run: `node scripts/docs-sync-audit.js --out-dir tests/reports/docs-sync-audit --skip-links`

Expected: 指南审计不产生 high 缺陷。实际结果：`agent-guide defects=0`；完整审计的 12 个 high 缺陷均为既有文档更新时间超阈值。
