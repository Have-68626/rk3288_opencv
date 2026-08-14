// 治理任务 SEC-14（S-SEC-B10-005）：workflow Action 引用固定门禁。
// 远程 GitHub Action 必须以完整 40 位 commit SHA 固定（@v4 / @main / 短 SHA 均为
// 可移动输入，属供应链漂移面）。本测试：
//   1. 纯函数单元测试（fixture 注入，不依赖仓库状态）；
//   2. 真实 ci.yml / docs-audit.yml 集成断言 —— RED 阶段（tag 引用存在）必须失败，
//      GREEN 阶段（已固定为 SHA）必须通过；
//   3. 直接运行 scripts/check-workflow-pins.js 二进制，断言退出码 0。
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const test = require("node:test");

const {
  parseUsesValue,
  splitUsesValue,
  classifyRef,
  scanWorkflow,
  auditWorkflowDir,
} = require("../../scripts/check-workflow-pins.js");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const CI_YML = path.join(REPO_ROOT, ".github", "workflows", "ci.yml");
const DOCS_AUDIT_YML = path.join(REPO_ROOT, ".github", "workflows", "docs-audit.yml");

// ---------- 纯函数单元测试 ----------

test("SEC-14：parseUsesValue 提取 uses 引用值", () => {
  assert.equal(parseUsesValue("        uses: actions/checkout@v4"), "actions/checkout@v4");
  assert.equal(parseUsesValue("      - uses: actions/setup-node@v4"), "actions/setup-node@v4");
  assert.equal(parseUsesValue("        'uses': actions/cache@v4"), "actions/cache@v4");
  // 行尾注释允许（固定后保留 tag 可读性）
  assert.equal(parseUsesValue("        uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4"),
    "actions/checkout@11d5960a326750d5838078e36cf38b85af677262");
  // 非 uses 键 / 注释行不匹配
  assert.equal(parseUsesValue("        run: echo \"uses: actions/x@v1\""), null);
  assert.equal(parseUsesValue("        # uses: actions/checkout@v4"), null);
  assert.equal(parseUsesValue("      - name: Checkout"), null);
});

test("SEC-14：splitUsesValue 区分远程/docker/本地引用", () => {
  assert.deepEqual(splitUsesValue("actions/checkout@v4"), { kind: "action", ownerRepo: "actions/checkout", ref: "v4" });
  assert.deepEqual(splitUsesValue("docker://alpine:3.18"), { kind: "docker", ownerRepo: null, ref: null });
  assert.deepEqual(splitUsesValue("./local-action"), { kind: "local", ownerRepo: null, ref: null });
  assert.deepEqual(splitUsesValue("actions/no-ref"), { kind: "no-ref", ownerRepo: "actions/no-ref", ref: null });
});

test("SEC-14：classifyRef 仅完整 40 位 SHA 视为 full-sha", () => {
  assert.equal(classifyRef("11d5960a326750d5838078e36cf38b85af677262"), "full-sha");
  assert.equal(classifyRef("v4"), "mutable");
  assert.equal(classifyRef("v3.5.0"), "mutable");
  assert.equal(classifyRef("main"), "mutable");
  assert.equal(classifyRef("11d5960a"), "short-sha");
});

test("SEC-14：scanWorkflow 跳过 docker/本地与 run 块内文本，标记 tag/短SHA", () => {
  const src = [
    "name: demo",
    "on:",
    "  pull_request:",
    "jobs:",
    "  build:",
    "    runs-on: ubuntu-latest",
    "    steps:",
    "      - uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4",
    "      - name: Echo",
    "        run: |",
    '          echo "uses: actions/evil@v1"',
    "          echo \"not a key: value\"",
    "      - uses: docker://alpine:3.18",
    "      - uses: ./local-action",
    "      - uses: actions/checkout@v4",
    "      - uses: actions/cache@0057852bfaa89a56745cba8c7296529d2fc39830",
  ].join("\n");
  const entries = scanWorkflow(src);
  assert.equal(entries.length, 5, `应识别 5 处真实 uses，实际 ${entries.length}`);
  // run 块内的 "uses: actions/evil@v1" 不得被误判
  assert.ok(!entries.some((e) => e.value.includes("evil")), "run 块内文本不应被识别为 uses");
  const mutable = entries.filter((e) => e.verdict !== "ok");
  assert.equal(mutable.length, 1, `仅 actions/checkout@v4 应为可移动，实际 ${mutable.length}`);
  assert.equal(mutable[0].value, "actions/checkout@v4");
  assert.equal(mutable[0].line, 15);
});

test("SEC-14：block-scalar 结束（缩进回落）后 uses 恢复识别", () => {
  const src = [
    "    steps:",
    "      - name: X",
    "        run: |",
    "          echo hi",
    "      - uses: actions/checkout@v4",
  ].join("\n");
  const entries = scanWorkflow(src);
  assert.equal(entries.length, 1);
  assert.equal(entries[0].value, "actions/checkout@v4");
  assert.equal(entries[0].verdict, "mutable");
});

// ---------- 真实仓库集成断言（RED → GREEN） ----------

test("SEC-14：ci.yml 全部 uses 为完整 SHA（当前若为 tag 引用则 RED 失败）", async () => {
  const src = fs.readFileSync(CI_YML, "utf8");
  const mutable = scanWorkflow(src).filter((e) => e.verdict !== "ok");
  assert.equal(mutable.length, 0,
    `ci.yml 存在 ${mutable.length} 处可移动引用，必须固定为完整 40 位 SHA：\n` +
    mutable.map((m) => `  L${m.line} ${m.value} (${m.ref})`).join("\n"));
});

test("SEC-14：docs-audit.yml 全部 uses 为完整 SHA", async () => {
  const src = fs.readFileSync(DOCS_AUDIT_YML, "utf8");
  const mutable = scanWorkflow(src).filter((e) => e.verdict !== "ok");
  assert.equal(mutable.length, 0,
    `docs-audit.yml 存在 ${mutable.length} 处可移动引用：\n` +
    mutable.map((m) => `  L${m.line} ${m.value} (${m.ref})`).join("\n"));
});

test("SEC-14：门禁二进制退出码 0（可移动引用存在时 RED 非零）", () => {
  const r = spawnSync(process.execPath, [path.join(REPO_ROOT, "scripts", "check-workflow-pins.js")],
    { cwd: REPO_ROOT, encoding: "utf8" });
  assert.equal(r.status, 0, `check-workflow-pins.js 应退出 0，实际 ${r.status}\n${r.stdout}\n${r.stderr}`);
  assert.match(r.stdout, /ALL_PINNED/);
});

test("SEC-14：workflow 目录审计含两个 YAML 文件", async () => {
  const { files, mutable } = await auditWorkflowDir(REPO_ROOT);
  assert.ok(files.includes(".github/workflows/ci.yml"), "应覆盖 ci.yml");
  assert.ok(files.includes(".github/workflows/docs-audit.yml"), "应覆盖 docs-audit.yml");
  assert.equal(mutable.length, 0, `全目录可移动引用应清零，实际 ${mutable.length}`);
});
