// 治理任务 P3-10（文档/规格漂移聚合）：security-policy-audit.js 对
// docs/security/enablement-plan.md 从"仅存在非空"增强为章节/默认状态表锚点校验
// （与 SECURITY.md 同等强度）。本测试断言：
//   1) 增强后的锚点校验对真实仓库仍 GREEN（离线可跑、fail-closed 不误伤）；
//   2) enablement-plan.md 缺任一必需章节 / 默认状态表列 → RED（退出码 1）；
//   3) enablement-plan.md 缺失 → RED。
//
// 沿用 docs-sync-audit-gate.test.js 的夹具隔离模式：脚本 repoRoot 固定为
// path.resolve(__dirname, "..")，故把脚本复制进夹具的 scripts/ 子目录，使
// __dirname 指向夹具根，从而在隔离目录中运行完整 main()。
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const test = require("node:test");

const SCRIPT_SRC = path.join(__dirname, "..", "..", "scripts", "security-policy-audit.js");
const REPO_ROOT = path.resolve(__dirname, "..", "..");

function write(root, relativePath, text) {
  const target = path.join(root, relativePath);
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, text, "utf8");
}

// 满足 SECURITY.md 全部 4 个必需锚点的最小夹具。
function securityMd() {
  return [
    "# 安全策略（Security Policy）",
    "",
    "## 报告安全漏洞",
    "通过 GitHub issue 报告。",
    "",
    "## 安全边界",
    "本地服务仅 bind 127.0.0.1。",
    "",
    "## 安全功能清单与默认状态",
    "",
    "| 安全功能 | 默认状态 |",
    "|---|---|",
    "| REST API 认证 | 默认关闭 |",
    "",
    "## 文档映射",
    "",
    "- `docs/security/auth-model.md`",
    "",
  ].join("\n");
}

// 满足 enablement-plan 全部 3 个必需锚点的最小夹具（默认开/关清单表 + 启用建议 + 验收说明）。
function enablementPlan() {
  return [
    "# 最小启用方案（Security Enablement Plan）",
    "",
    "## 1. 默认开/关清单",
    "",
    "| 安全功能 | 当前状态 | 是否可关闭 | 说明 |",
    "|---|---|---|---|",
    "| REST API 认证 | 关（默认） | 是 | 兼容模式 |",
    "",
    "## 2. 启用建议（生产部署最小清单）",
    "",
    "1. 设置 `RK_WCFR_HTTP_AUTH_TOKEN`。",
    "",
    "## 3. 验收说明（本任务边界）",
    "",
    "本地文档与门禁验证。",
    "",
  ].join("\n");
}

function createCleanFixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "security-policy-audit-"));
  write(root, "scripts/security-policy-audit.js", fs.readFileSync(SCRIPT_SRC, "utf8"));
  write(root, "SECURITY.md", securityMd());
  write(root, "docs/security/enablement-plan.md", enablementPlan());
  return root;
}

// 以夹具为 cwd 运行夹具内的审计脚本，返回 spawnSync 结果。
function runAudit(root, extraArgs = []) {
  return spawnSync(process.execPath, [
    path.join(root, "scripts", "security-policy-audit.js"),
    "--ci",
    ...extraArgs,
  ], { cwd: root, encoding: "utf8" });
}

test("P3-10：真实仓库经增强锚点校验仍 GREEN（离线可跑、不误伤）", () => {
  const r = runAudit(REPO_ROOT);
  assert.equal(r.status, 0, `真实仓库应 GREEN；stdout=${r.stdout}\nstderr=${r.stderr}`);
  assert.match(r.stdout, /SECURITY_POLICY_GREEN/);
  // 增强后 enablement-plan 的三个锚点必须全部 PASS。
  assert.match(r.stdout, /SEC_AUDIT_PASS check=enablement name=默认开\/关清单章节/);
  assert.match(r.stdout, /SEC_AUDIT_PASS check=enablement-recommend name=启用建议章节/);
  assert.match(r.stdout, /SEC_AUDIT_PASS check=enablement-acceptance name=验收说明章节/);
});

test("P3-10：干净夹具应 GREEN（退出码 0）", () => {
  const root = createCleanFixture();
  try {
    const r = runAudit(root);
    assert.equal(r.status, 0, `stdout=${r.stdout}\nstderr=${r.stderr}`);
    assert.match(r.stdout, /SECURITY_POLICY_GREEN/);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("P3-10：enablement-plan 缺『验收说明』章节 → RED（退出码 1）", () => {
  const root = createCleanFixture();
  try {
    fs.writeFileSync(
      path.join(root, "docs/security/enablement-plan.md"),
      enablementPlan().replace(/## 3\. 验收说明[\s\S]*$/, "## 3. 结束\n"),
      "utf8");
    const r = runAudit(root);
    assert.notEqual(r.status, 0, `缺验收说明章节应 RED；stdout=${r.stdout}`);
    assert.match(r.stdout, /SECURITY_POLICY_RED/);
    assert.match(r.stdout, /check=enablement-acceptance .*缺少锚点/);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("P3-10：enablement-plan 默认状态表缺『当前状态』列 → RED（退出码 1）", () => {
  const root = createCleanFixture();
  try {
    fs.writeFileSync(
      path.join(root, "docs/security/enablement-plan.md"),
      enablementPlan().replace("当前状态", "状态"),
      "utf8");
    const r = runAudit(root);
    assert.notEqual(r.status, 0, `缺默认状态表列应 RED；stdout=${r.stdout}`);
    assert.match(r.stdout, /SECURITY_POLICY_RED/);
    assert.match(r.stdout, /check=enablement .*缺少锚点/);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("P3-10：enablement-plan.md 缺失 → RED（退出码 1）", () => {
  const root = createCleanFixture();
  try {
    fs.rmSync(path.join(root, "docs/security/enablement-plan.md"), { force: true });
    const r = runAudit(root);
    assert.notEqual(r.status, 0, `缺文件应 RED；stdout=${r.stdout}`);
    assert.match(r.stdout, /SECURITY_POLICY_RED/);
    assert.match(r.stdout, /check=enablement .*文件缺失或为空/);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});
